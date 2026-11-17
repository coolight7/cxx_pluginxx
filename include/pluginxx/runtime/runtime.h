/// pluginxx 插件框架运行时 (实例状态机 / 执行 lease / 投递通道)
///
/// 定位: 与主程序业务无关的插件框架运行时, 供各主程序复用。
/// 所有类型仅供主程序内部使用, 不属于 C ABI。
///
/// 内容:
/// - [PluginRuntime]: 不捕获 manager 裸指针的公共运行时状态 (io executor / 插件
///   executor / 操作表 / 待重放动作队列), 由主程序管理器持有;
/// - [InstanceLifetime] / [InstanceLease]: 实例状态机与执行 lease (关闭等待覆盖
///   所有已进入插件代码的执行);
/// - [enqueueRuntimeAction] / [replayRuntimeActions]: 经 io executor 投递动作, 并在
///   executor 停止期间保留、恢复后重放;
/// - [enqueuePluginAction] / [replayPluginActions]: 同上, 但投到**插件执行器** (所有
///   "主程序调用插件" 的回调都从这里进插件代码);
/// - [pluginExecutionExecutor] / [isRuntimePluginThread]: 插件执行器的取值与线程判定。
///
/// 两个执行器:
/// - `executor` (io): 主程序状态机 (会话/注册表/图/定时器) 与全部受理动作;
/// - `pluginExecutor` (插件线程): 插件代码 (回调/协程驱动/插件侧取消) 只在这条线程上
///   执行; 为空表示宿主没有起插件线程, 此时插件代码与 io 同线程 (行为与改造前一致)。
#pragma once

#include "utilxx_base/asio_error.h"
#include "utilxx_base/log.h"
#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/execution.hpp"
#include "asio/io_context.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/// 一次 Operation 的 ABI 句柄 (定义在 `pluginxx/runtime/op_driver.h`; 位于**全局**命名空间)
struct PluginxxOperatorHandle;

namespace pluginxx {

using RuntimeErrorCode = utilxx_base::AsioErrorCode;

struct PluginRuntime;

/// 运行时动作可能同时被"旧 executor 队列"和"新 executor 上的重放"看到;
/// claim 位让这场竞争只产生一次执行, 且无需取消已经投递的 handler。
struct RuntimeAction {
    std::atomic<bool>     claimed{false};
    std::function<void()> fn;
};

bool enqueueRuntimeAction(
    const std::shared_ptr<PluginRuntime>& runtime,
    std::function<void()>                 fn,
    bool                                  retainWhenStopped
) noexcept;
void replayRuntimeActions(const std::shared_ptr<PluginRuntime>& runtime) noexcept;
bool enqueuePluginAction(
    const std::shared_ptr<PluginRuntime>& runtime,
    std::function<void()>                 fn,
    bool                                  retainWhenStopped
) noexcept;
void replayPluginActions(const std::shared_ptr<PluginRuntime>& runtime) noexcept;

enum class PluginInstanceState : uint32_t {
    Loading,
    Ready,
    Disabled,
    Closing,
    Closed,
    CloseFailed,
};

inline const char* pluginInstanceStateName(PluginInstanceState state) noexcept {
    switch (state) {
        case PluginInstanceState::Loading:
            return "loading";
        case PluginInstanceState::Ready:
            return "ready";
        case PluginInstanceState::Disabled:
            return "disabled";
        case PluginInstanceState::Closing:
            return "closing";
        case PluginInstanceState::Closed:
            return "closed";
        case PluginInstanceState::CloseFailed:
            return "close_failed";
    }
    return "unknown";
}

/// 实例状态与执行 lease 的公共控制块。
/// - 状态变更与 idle 等待者列表只在 IO 线程访问。
/// - admission 位与计数共用一次 CAS，关闭与跨线程获取 lease 之间没有空隙。
/// - 最后一个 lease 释放后向 IO 线程投递一次通知，唤醒全部 idle 等待者。
class InstanceLifetime : public std::enable_shared_from_this<InstanceLifetime> {
public:

    using ActionEnqueuer = std::function<bool(std::function<void()>)>;

    InstanceLifetime(asio::any_io_executor executor, std::string name, uint64_t generation) :
        executor_(std::move(executor)),
        name_(std::move(name)),
        generation_(generation) {}

    InstanceLifetime(
        asio::any_io_executor executor,
        std::string           name,
        uint64_t              generation,
        ActionEnqueuer        enqueue
    ) :
        executor_(std::move(executor)),
        name_(std::move(name)),
        generation_(generation),
        enqueue_(std::move(enqueue)) {}

    InstanceLifetime(const InstanceLifetime&)            = delete;
    InstanceLifetime& operator=(const InstanceLifetime&) = delete;

    const std::string& name() const noexcept {
        return name_;
    }

    uint64_t generation() const noexcept {
        return generation_;
    }

    PluginInstanceState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    /// 仅所属 IO 线程调用；Closed 不得重新打开，重载必须建立新的 lifetime。
    void setState(PluginInstanceState next) noexcept {
        assert(state() != PluginInstanceState::Closed || next == PluginInstanceState::Closed);
        if (next == PluginInstanceState::Loading || next == PluginInstanceState::Ready) {
            state_.store(next, std::memory_order_release);
            leases_.fetch_and(kCountMask, std::memory_order_acq_rel);
        } else {
            leases_.fetch_or(kNoAdmission, std::memory_order_acq_rel);
            state_.store(next, std::memory_order_release);
        }
    }

    bool acceptsOperations() const noexcept {
        return state() == PluginInstanceState::Ready;
    }

    bool acceptsRegistration() const noexcept {
        const auto s = state();
        return s == PluginInstanceState::Loading || s == PluginInstanceState::Ready;
    }

    void requestClose() noexcept {
        setState(PluginInstanceState::Closing);
    }

    bool closeRequested() const noexcept {
        const auto s = state();
        return s == PluginInstanceState::Closing || s == PluginInstanceState::CloseFailed
               || s == PluginInstanceState::Closed;
    }

    size_t leaseCount() const noexcept {
        return static_cast<size_t>(leases_.load(std::memory_order_acquire) & kCountMask);
    }

    /// 设置一次性 idle 收尾动作。动作只在所属 IO 线程执行，适用于 manager
    /// 已经开始关闭但自身 owner 即将析构的场景；动作执行前会从 lifetime
    /// 中取出，避免 lifetime 与插件实例形成永久循环引用。
    bool setIdleCleanup(std::function<void()> cleanup) {
        if (!cleanup || idleCleanup_ || idleCleanupRegistered_) {
            return false;
        }
        idleCleanupRegistered_ = true;
        // 关闭请求与最后一个 lease 释放在同一 IO 线程串行时通常不会走到这里，
        // 但保留这个分支可处理调用方观察到 idle 后才登记收尾的边界。
        if (leaseCount() == 0) {
            idleCleanup_        = std::move(cleanup);
            const auto weakSelf = weak_from_this();
            if (enqueueAction([weakSelf] {
                    if (auto self = weakSelf.lock()) {
                        self->publishIdle();
                    }
                })) {
                return true;
            }
            XX_LOGE("Plugin `{}` failed to publish idle cleanup", name_);
            return false;
        }
        idleCleanup_ = std::move(cleanup);
        return true;
    }

    /// 仅供测试/直接收尾路径取消尚未执行的 idle 动作。
    void clearIdleCleanup() noexcept {
        idleCleanup_           = {};
        idleCleanupRegistered_ = false;
    }

    /// `lifecycle`: 仅 IO 线程用于主程序显式 start/stop 调用；不得供新业务操作使用。
    bool tryAcquire(bool lifecycle = false) noexcept {
        auto count = leases_.load(std::memory_order_acquire);
        for (;;) {
            if ((!lifecycle && (count & kNoAdmission)) || state() == PluginInstanceState::Closed
                || (count & kCountMask) == kCountMask) {
                return false;
            }
            if (leases_.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel)) {
                return true;
            }
        }
    }

    void release() noexcept {
        const auto previous = leases_.fetch_sub(1, std::memory_order_acq_rel);
        assert((previous & kCountMask) != 0);
        if ((previous & kCountMask) == 1) {
            const auto weakSelf = weak_from_this();
            if (!enqueueAction([weakSelf] {
                    if (auto self = weakSelf.lock()) {
                        self->publishIdle();
                    }
                })) {
                // 不从释放 lease 的 worker 线程调用任何插件代码。若 executor
                // 停止，runtime action 会保留闭包并在恢复时重放。
                XX_LOGE("Plugin `{}` failed to publish idle event", name_);
            }
        }
    }

    /// 无轮询的绝对截止等待；并行等待者各有自己的 timer，通知会广播。
    asio::awaitable<bool> waitIdleUntil(std::chrono::steady_clock::time_point deadline) {
        auto self = shared_from_this();
        if (leaseCount() == 0) {
            co_return true;
        }
        auto waiter = std::make_shared<IdleWaiter>(executor_, deadline);
        std::erase_if(idleWaiters_, [](const auto& w) {
            return w.expired();
        });
        idleWaiters_.push_back(waiter);
        auto [ec] = co_await waiter->timer.async_wait(asio::as_tuple(asio::use_awaitable));
        std::erase_if(idleWaiters_, [&waiter](const auto& w) {
            auto p = w.lock();
            return !p || p == waiter;
        });
        if (ec && !waiter->idle) {
            throw utilxx_base::AsioSystemError(ec);
        }
        co_return waiter->idle || leaseCount() == 0;
    }

private:

    struct IdleWaiter {
        asio::steady_timer timer;
        bool               idle = false;

        IdleWaiter(
            const asio::any_io_executor&          ex,
            std::chrono::steady_clock::time_point deadline
        ) :
            timer(ex, deadline) {}
    };

    static constexpr uint64_t kNoAdmission = uint64_t{1} << 63;
    static constexpr uint64_t kCountMask   = kNoAdmission - 1;

    bool enqueueAction(std::function<void()> fn) noexcept {
        if (enqueue_) {
            return enqueue_(std::move(fn));
        }
        try {
            if (!executor_) {
                return false;
            }
            asio::post(executor_, [fn = std::move(fn)]() mutable {
                try {
                    fn();
                } catch (const std::exception& e) {
                    XX_LOGE("Plugin idle action threw: {}", e.what());
                } catch (...) {
                    XX_LOGE("Plugin idle action threw unknown exception");
                }
            });
            return true;
        } catch (...) {
            return false;
        }
    }

    bool publishIdle() noexcept {
        if (leaseCount() != 0) {
            return true;
        }
        auto waiters = std::move(idleWaiters_);
        idleWaiters_.clear();
        for (const auto& weak : waiters) {
            if (auto waiter = weak.lock()) {
                waiter->idle = true;
                waiter->timer.cancel();
            }
        }
        auto cleanup = std::move(idleCleanup_);
        idleCleanup_ = {};
        if (cleanup) {
            try {
                cleanup();
            } catch (const std::exception& e) {
                XX_LOGE("Plugin `{}` idle cleanup threw: {}", name_, e.what());
            } catch (...) {
                XX_LOGE("Plugin `{}` idle cleanup threw unknown exception", name_);
            }
        }
        return true;
    }

    asio::any_io_executor                  executor_;
    std::string                            name_;
    uint64_t                               generation_;
    std::atomic<PluginInstanceState>       state_{PluginInstanceState::Loading};
    std::atomic<uint64_t>                  leases_{0};
    std::vector<std::weak_ptr<IdleWaiter>> idleWaiters_;
    std::function<void()>                  idleCleanup_;
    ActionEnqueuer                         enqueue_;
    bool                                   idleCleanupRegistered_ = false;
};

class InstanceLease {
public:

    InstanceLease() = default;

    static InstanceLease acquire(
        const std::shared_ptr<InstanceLifetime>& lifetime,
        bool                                     lifecycle = false
    ) noexcept {
        return lifetime && lifetime->tryAcquire(lifecycle) ? InstanceLease(lifetime)
                                                           : InstanceLease{};
    }

    ~InstanceLease() {
        reset();
    }

    InstanceLease(const InstanceLease&)            = delete;
    InstanceLease& operator=(const InstanceLease&) = delete;

    InstanceLease(InstanceLease&& other) noexcept :
        lifetime_(std::move(other.lifetime_)) {}

    InstanceLease& operator=(InstanceLease&& other) noexcept {
        if (this != &other) {
            reset();
            lifetime_ = std::move(other.lifetime_);
        }
        return *this;
    }

    explicit operator bool() const noexcept {
        return lifetime_ != nullptr;
    }

    void reset() noexcept {
        if (auto lifetime = std::exchange(lifetime_, {})) {
            lifetime->release();
        }
    }

private:

    explicit InstanceLease(std::shared_ptr<InstanceLifetime> lifetime) :
        lifetime_(std::move(lifetime)) {}

    std::shared_ptr<InstanceLifetime> lifetime_;
};

struct OpCore;

/// 一次插件回调的耗时统计 (跨实例汇总; 每次派发更新)
///
/// 只在插件执行器空闲可用时计算: 排队时长不计入回调耗时, 因为队头阻塞本身就是
/// "某个别的回调跑久了"的症状, 记在同一个数上会让定位失去指向性。
struct PluginCallbackStats {
    /// 已执行的回调数
    std::atomic<uint64_t> callbacks{0};
    /// 起止时间跨度累计 (微秒): 除以 callbacks 得到平均回调耗时
    std::atomic<uint64_t> totalMicros{0};
    /// 最长的一次回调耗时 (微秒)
    std::atomic<uint64_t> maxMicros{0};
    /// 超过预算的回调数
    std::atomic<uint64_t> overBudget{0};
    /// 派发时拿不到实例 lease 而丢弃的回调数 (实例已在关闭)
    std::atomic<uint64_t> dropped{0};
    /// 插件执行器不可用 (缺失/已停止) 而丢弃的派发数
    std::atomic<uint64_t> unschedulable{0};
    /// 单次回调耗时预算 (毫秒); 超过记 WARN 并计入 [overBudget]
    std::atomic<uint32_t> budgetMs{100};
};

/// 不捕获 manager 裸指针的公共运行时状态。Operation 表只由 IO 线程操作。
/// 已接受 Operation 持有 runtime，runtime 持有 Operation，直到最终清理时断开；
/// 等待者取消或 manager 被错误销毁不能提前释放仍在执行的 DSO/context。
struct PluginRuntime {
    asio::any_io_executor        executor;
    std::atomic<std::thread::id> ioThreadId{};
    /// 插件执行器 (所有插件代码在这里执行); 空 = 与 [executor] 同一个
    ///
    /// 绑定只发生在初始化阶段 (管理器建立时), 之后只读, 因此不需要锁。
    asio::any_io_executor pluginExecutor;
    /// 插件执行器的工作线程; 由投递闭包在目标线程上记录 (绑定阶段未知)
    std::atomic<std::thread::id> pluginThreadId{};
    /// 插件回调的耗时统计
    PluginCallbackStats callbackStats{};
    /// 正常路径只在 IO 线程访问；executor 停止后，完成线程仍可能需要
    /// 收束一个已接受 Operation，因此为这条故障路径提供最小互斥保护。
    mutable std::mutex                          operationsMutex;
    std::map<uint64_t, std::shared_ptr<OpCore>> operations;
    mutable std::mutex                          pendingMutex;
    std::deque<std::shared_ptr<RuntimeAction>>  pendingActions;
    /// 插件执行器停止期间保留的派发项 (与 io 那条队列分开, 恢复时各自重放)
    mutable std::mutex                          pendingPluginMutex;
    std::deque<std::shared_ptr<RuntimeAction>>  pendingPluginActions;
    uint64_t                                    nextOperationId = 1;
    uint64_t                                    nextGeneration  = 1;

    /// 无主操作 (主程序自己发起、没有实例租约) 的句柄保活表
    ///
    /// 普通的 Operation 由所属实例的 `operatorHandles` 保活; 无主操作没有实例, 而调用方
    /// 手里的裸指针要能安全地再传给取消入口 (已完成时是空操作), 所以由运行时自己保活。
    /// 只在 IO 线程读写; 长度超过 [kDetachedHandleKeep] 时先清已完成项。
    std::vector<std::shared_ptr<::PluginxxOperatorHandle>> detachedHandles;
    static constexpr size_t                              kDetachedHandleKeep = 512;

    /// 尚未终结（未提交完成）的 Operation 摘要，形如 `label#id, label#id`。
    ///
    /// 用途：关闭超时（CloseFailed）时给出"到底是哪些操作没终结"的可观察信息。
    /// 完成包在 executor 停止期间会保留在待重放队列里，此时 Operation 仍是未终结
    /// 状态；这个方法就是它的外部可观察表示。空串表示没有未终结操作。
    ///
    /// 定义见 `pluginxx/runtime/op_driver.h`（此处 OpCore 只有前置声明）。
    std::string pendingOperationSummary() const;
};

inline bool runtimeExecutorStopped(const asio::any_io_executor& executor) noexcept {
    if (!executor) {
        return true;
    }
    try {
        // runtime 支持的绑定是 io_context 的 executor; 类型擦除后的
        // execution_context 没有多态接口, 因此这里不用 RTTI 判断。
        // 其它 executor 适配器依然可用, 只是其持有者需要在同步调用前完成重绑定。
        if (const auto* ioExecutor = executor.target<asio::io_context::executor_type>()) {
            return ioExecutor->context().stopped();
        }
    } catch (...) {
        return true;
    }
    return false;
}

/// 当前线程是否为该运行时的 IO 线程 (仅用于断言与诊断)
/// - executor 缺失或已停止时返回 false (与 [PluginManagerBase::isIoThread] 同一套规则):
///   io_context 停止后即使当前线程正是最后绑定 executor 的线程, 也不得视为 io 线程,
///   否则同步 ABI 调用会在已关闭的 runtime 上继续执行
inline bool isRuntimeIoThread(const std::shared_ptr<PluginRuntime>& runtime) noexcept {
    if (!runtime || !runtime->executor || runtimeExecutorStopped(runtime->executor)) {
        return false;
    }
    const auto tid = runtime->ioThreadId.load(std::memory_order_acquire);
    return tid != std::thread::id{} && tid == std::this_thread::get_id();
}

/// 宿主是否给这个运行时配了**独立的**插件执行器
///
/// - false: 插件代码与 io 同线程 (与改造前一致, 直接 new 管理器嵌入的宿主就是这种);
/// - true: 插件代码只在 [PluginRuntime::pluginExecutor] 上执行。
inline bool hasPluginExecutor(const std::shared_ptr<PluginRuntime>& runtime) noexcept {
    return runtime && static_cast<bool>(runtime->pluginExecutor);
}

/// 插件代码的执行执行器 (未配独立插件执行器时退回 io executor)
inline asio::any_io_executor pluginExecutionExecutor(const std::shared_ptr<PluginRuntime>& runtime
) noexcept {
    if (!runtime) {
        return {};
    }
    return runtime->pluginExecutor ? runtime->pluginExecutor : runtime->executor;
}

/// 当前线程是不是这个运行时的插件执行线程
///
/// 语义: "在这里阻塞会不会把插件代码卡住"。有独立插件执行器时比对插件线程;
/// 没有时插件代码就跑在 io 线程上, 退回 [isRuntimeIoThread] 的结论。
/// kit 的同步桥 (`tableOpSyncWait`) 与 `scheduler.is_io_thread` 用这个判定。
inline bool isRuntimePluginThread(const std::shared_ptr<PluginRuntime>& runtime) noexcept {
    if (!runtime) {
        return false;
    }
    if (hasPluginExecutor(runtime)) {
        if (runtimeExecutorStopped(runtime->pluginExecutor)) {
            return false;
        }
        const auto tid = runtime->pluginThreadId.load(std::memory_order_acquire);
        return tid != std::thread::id{} && tid == std::this_thread::get_id();
    }
    return isRuntimeIoThread(runtime);
}

/// 绑定插件执行器 (**初始化阶段, 只调一次**)
///
/// - 传入空 executor 等于取消绑定, 插件代码回到 io 线程执行;
/// - 插件线程 id 在这里清空, 由第一次投递在实际工作线程上记录 (绑定调用方自己不
///   一定是那条线程);
/// - 绑定后重放上一轮停止期间保留的派发项。
inline void bindPluginExecutor(
    const std::shared_ptr<PluginRuntime>& runtime,
    asio::any_io_executor                 executor
) noexcept {
    if (!runtime) {
        return;
    }
    runtime->pluginExecutor = std::move(executor);
    runtime->pluginThreadId.store(std::thread::id{}, std::memory_order_release);
    if (runtime->pluginExecutor && !runtimeExecutorStopped(runtime->pluginExecutor)) {
        replayPluginActions(runtime);
    }
}

/// 待重放队列的选择 (两条队列的登记/摘除逻辑完全对称, 只差落哪个成员)
enum class RuntimeQueue : uint8_t {
    Io,
    Plugin,
};

inline std::mutex& runtimeQueueMutex(PluginRuntime& runtime, RuntimeQueue queue) noexcept {
    return queue == RuntimeQueue::Io ? runtime.pendingMutex : runtime.pendingPluginMutex;
}

inline std::deque<std::shared_ptr<RuntimeAction>>&
    runtimeQueueOf(PluginRuntime& runtime, RuntimeQueue queue) noexcept {
    return queue == RuntimeQueue::Io ? runtime.pendingActions : runtime.pendingPluginActions;
}

inline void runRuntimeAction(
    const std::weak_ptr<PluginRuntime>&   weakRuntime,
    const std::shared_ptr<RuntimeAction>& action,
    RuntimeQueue                          queue = RuntimeQueue::Io
) noexcept {
    if (!action || action->claimed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (auto runtime = weakRuntime.lock()) {
        std::lock_guard lock(runtimeQueueMutex(*runtime, queue));
        std::erase(runtimeQueueOf(*runtime, queue), action);
        if (queue == RuntimeQueue::Plugin) {
            // 插件执行器的工作线程只在这里得知 (绑定发生在初始化阶段, 那时还不知道
            // 哪条线程会跑)。`isPluginThread` / `scheduler.is_io_thread` 靠它回答。
            runtime->pluginThreadId.store(std::this_thread::get_id(), std::memory_order_release);
        }
    }
    try {
        if (action->fn) {
            action->fn();
        }
    } catch (const std::exception& e) {
        XX_LOGE("Plugin runtime action threw: {}", e.what());
    } catch (...) {
        XX_LOGE("Plugin runtime action threw unknown exception");
    }
}

/// 投递到指定执行器, 并把动作登记在指定队列里 (停止期间保留 / 恢复后重放)
inline bool enqueueRuntimeActionOn(
    const std::shared_ptr<PluginRuntime>& runtime,
    asio::any_io_executor                 executor,
    RuntimeQueue                          queue,
    std::function<void()>                 fn,
    bool                                  retainWhenStopped
) noexcept {
    if (!runtime || !fn) {
        return false;
    }
    auto action = std::make_shared<RuntimeAction>();
    action->fn  = std::move(fn);
    if (!executor) {
        if (!retainWhenStopped) {
            return false;
        }
        std::lock_guard lock(runtimeQueueMutex(*runtime, queue));
        runtimeQueueOf(*runtime, queue).push_back(action);
        return true;
    }
    if (!retainWhenStopped && runtimeExecutorStopped(executor)) {
        return false;
    }
    // 先登记再投递: io_context 可能在探测与 asio::post 之间转为 stopped
    // (自然空闲的 context 同样报告 stopped), 此时已投递的 handler 与这里的
    // 待执行记录通过 RuntimeAction::claimed 安全竞争, 只会执行一次。
    if (retainWhenStopped) {
        std::lock_guard lock(runtimeQueueMutex(*runtime, queue));
        runtimeQueueOf(*runtime, queue).push_back(action);
    }
    try {
        const std::weak_ptr<PluginRuntime> weakRuntime = runtime;
        asio::post(executor, [weakRuntime, action, queue] {
            runRuntimeAction(weakRuntime, action, queue);
        });
        return true;
    } catch (...) {
        if (!retainWhenStopped) {
            return false;
        }
        std::lock_guard lock(runtimeQueueMutex(*runtime, queue));
        auto&           pending = runtimeQueueOf(*runtime, queue);
        if (std::find(pending.begin(), pending.end(), action) == pending.end()) {
            pending.push_back(std::move(action));
        }
        return true;
    }
}

inline bool enqueueRuntimeAction(
    const std::shared_ptr<PluginRuntime>& runtime,
    std::function<void()>                 fn,
    bool                                  retainWhenStopped
) noexcept {
    return enqueueRuntimeActionOn(
        runtime,
        runtime ? runtime->executor : asio::any_io_executor{},
        RuntimeQueue::Io,
        std::move(fn),
        retainWhenStopped
    );
}

/// 投递一段**插件代码**到插件执行器 (未配独立执行器时退回 io executor)
///
/// 与 [enqueueRuntimeAction] 的差别只有落到哪个执行器/队列: 两个方向共用同一套
/// "停止期间保留 + 恢复后重放 + 只执行一次" 语义。
inline bool enqueuePluginAction(
    const std::shared_ptr<PluginRuntime>& runtime,
    std::function<void()>                 fn,
    bool                                  retainWhenStopped
) noexcept {
    return enqueueRuntimeActionOn(
        runtime,
        pluginExecutionExecutor(runtime),
        RuntimeQueue::Plugin,
        std::move(fn),
        retainWhenStopped
    );
}

/// 同上, 但显式指定执行器 (实例自带执行器的预留入口)
inline bool enqueuePluginActionOn(
    const std::shared_ptr<PluginRuntime>& runtime,
    asio::any_io_executor                 executor,
    std::function<void()>                 fn,
    bool                                  retainWhenStopped
) noexcept {
    return enqueueRuntimeActionOn(
        runtime,
        std::move(executor),
        RuntimeQueue::Plugin,
        std::move(fn),
        retainWhenStopped
    );
}

inline void replayRuntimeActionsInternal(
    const std::shared_ptr<PluginRuntime>& runtime,
    asio::any_io_executor                 executor,
    RuntimeQueue                          queue
) noexcept {
    if (!runtime || !executor || runtimeExecutorStopped(executor)) {
        return;
    }
    std::vector<std::shared_ptr<RuntimeAction>> pending;
    {
        std::lock_guard lock(runtimeQueueMutex(*runtime, queue));
        auto&           source = runtimeQueueOf(*runtime, queue);
        pending.reserve(source.size());
        for (const auto& action : source) {
            if (action && !action->claimed.load(std::memory_order_acquire)) {
                pending.push_back(action);
            }
        }
    }
    for (const auto& action : pending) {
        try {
            const std::weak_ptr<PluginRuntime> weakRuntime = runtime;
            asio::post(executor, [weakRuntime, action, queue] {
                runRuntimeAction(weakRuntime, action, queue);
            });
        } catch (...) { /* retain the action for the next executor binding */
        }
    }
}

inline void replayRuntimeActions(const std::shared_ptr<PluginRuntime>& runtime) noexcept {
    replayRuntimeActionsInternal(
        runtime,
        runtime ? runtime->executor : asio::any_io_executor{},
        RuntimeQueue::Io
    );
}

inline void replayPluginActions(const std::shared_ptr<PluginRuntime>& runtime) noexcept {
    replayRuntimeActionsInternal(runtime, pluginExecutionExecutor(runtime), RuntimeQueue::Plugin);
}

} // namespace pluginxx
