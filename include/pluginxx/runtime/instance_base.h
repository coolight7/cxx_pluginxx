/// pluginxx 插件实例基类与主程序视图控制块 (主程序侧, 与主程序业务无关)
///
/// 内容:
/// - [PluginInstanceBase]: 实例公共基类 (元信息/依赖/启用标志/主程序句柄/驱动登记表/
///   执行 lease/destroy 执行), 主程序各端的实例各自继承;
/// - [PluginHostControl]: 交给插件的 `PluginxxHost*` 视图所在的控制块 —— 地址
///   永不失效, 实例关闭后只清空实例引用 (tombstone), 因此插件跨卸载持有旧 host 指针
///   时各 vtable 入口只会安全失败, 既不访问已释放对象也不误指新实例;
/// - [resolvePluginHostControl]: 反查插件传入的 host 视图对应控制块;
/// - [hostMemoryAlloc] / [hostMemoryFree] / [hostMemoryCreateString] /
///   [hostMemorySetString]: C ABI 跨 CRT 堆内存操作与主程序堆字符串构造;
/// - [getExecutableDirPath]: 跨平台可执行目录 helper (builtin:// 回退探测用)。
///
/// 线程约定: 实例字段 (注册记录/依赖表) 仅 IO 线程读写; `enabled` 为原子布尔
/// (插件线程受理操作时要读它), 操作登记表 ([recordsMutex]) 与驱动登记表
/// 各用独立互斥 (允许任意线程建/取消)。
#pragma once

#include "asio/any_io_executor.hpp"
#include "pluginxx/api/abi.h"
#include "pluginxx/host/capability_registry.h"
#include "pluginxx/host/broadcast_registry.h"
#include "pluginxx/host/loader.h"
#include "pluginxx/runtime/driver.h"
#include "pluginxx/runtime/runtime.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct PluginxxOperatorHandle;
struct PluginxxOperationCompletionEndpoint;

namespace pluginxx {

class PluginHostControl;

// =====================================================================
// 实例公共基类
// =====================================================================

/// 插件实例公共基类 (主程序各端的实例类继承)
/// - 持有跨端一致的元信息/依赖/启用标志/执行中计数/主程序句柄
/// - InflightGuard 为公共 RAII (事件 handler / 命令 execute / 异步 op 入口计数)
struct PluginInstanceBase {
    std::string name;        ///< 唯一标识 (与对端插件共用命名空间)
    std::string version;     ///< 版本号 (get_info 或默认)
    std::string description; ///< 描述
    std::string path;        ///< 加载的库路径/内置路径
    /// 插件配置参数 (yaml `plugins` 条目 args; 主程序原样保存, 经 vtable
    /// get_plugin_args 整体返回给插件, 不解析其字段语义)
    utilxx_base::Json args = utilxx_base::Json::object();
    /// 插件配置文件所在目录或文件路径 (yaml `config`, 归一化为绝对路径)
    std::string              configPath;
    std::vector<std::string> depends; ///< 必选依赖 (未安装加载失败; 卸载/禁用级联)
    std::vector<std::string> optionalDepends;     ///< 可选依赖 (未安装仅警告)
    void*                    dlHandle  = nullptr; ///< dlopen/LoadLibrary 句柄
    void*                    pluginCtx = nullptr; ///< entry 输出的插件私有上下文
    /// 内置插件 (合并编译进主程序二进制) 的 destroy 入口; 动态库插件为 nullptr
    /// (此时 destroy 由 [destroyPlugin] 经 [pluginDestroySymbol] 向动态库查找)
    PluginxxDestroyFn builtinUnload = nullptr;
    /// 是否启用 (禁用: 注册摘除/命令停用)
    ///
    /// 原子: 由 IO 线程写入, 插件线程受理操作 (`sleep`/`offload`/`register_task`/
    /// 能力调用) 时要读它判"还能不能起新动作"。
    std::atomic<bool> enabled{true};
    bool              userDisabled = false; ///< 是否被用户显式禁用 (区别于级联禁用)
    bool blockedByDependencies     = false; ///< 是否因必选依赖不可用而级联禁用
    bool unloadRequested           = false; ///< 已请求卸载 (防重复)
    /// create 是否成功产出可销毁的 pluginCtx。
    bool pluginCreated = false;
    /// 插件上下文是否已经调用 destroy。只在所属 IO 线程更新。
    bool pluginDestroyed = false;
    /// 同步关闭发现活动 lease 时，等待最后一个 lease 释放后再执行 destroy。
    bool destroyDeferred = false;
    /// 实例的创建 / 停用 / 销毁入口 (加载成功的插件必有这三个符号, 见 plugin_api.h):
    /// - create: 构造 (首次) + 注册事务; 可重入 (第二次只做注册);
    /// - stop: 撤销注册与自管资源, 上下文保留 (可以再次 create 重新注册);
    /// - destroy: 释放本地对象, 完成后主程序才 dlclose。
    ///
    /// 三个入口都是"调用 → 恰好一次完成通知"的协议, 由
    /// [pluginxx::awaitPluginLifecycleVoid] 驱动。
    PluginxxCreateFn  lifecycleCreate  = nullptr;
    PluginxxStopFn    lifecycleStop    = nullptr;
    PluginxxDestroyFn lifecycleDestroy = nullptr;
    /// 注册事务 (create) 是否已成功完成 (加载成功即置位)。
    bool lifecycleStarted = false;
    /// stop 事务是否已执行完成 (destroy 的前提)。
    bool lifecycleStopped = false;

    /// 日志前缀 (借用它的主程序实例类可覆写以区分日志来源; 默认无前缀)
    virtual std::string_view logTag() const noexcept {
        return {};
    }

    /// 销毁结束后的收尾: 上下文作废 + 退休框架控制块
    ///
    /// 调用时机: 插件的 destroy 入口已经完成后 (活动 lease 已归零, 插件代码
    /// 不会再执行), 或插件从未建出上下文。调用后插件持有的旧 host 指针只能
    /// 安全失败, 旧 ctx 指针不得再使用。
    void finishDestroy() noexcept;

    /// 关闭动态库句柄, 释放主程序对插件文件的占用。
    ///
    /// **只能在插件代码不会再执行时调用**: stop 事务已完成、活动 lease 已归零、
    /// 实例也已从插件表摘除。Windows 上没关掉的句柄会一直映射着插件文件, 插件目录
    /// 删不掉、库文件也换不掉 (表现为"拒绝访问"), 卸载与插件更新都做不下去。
    ///
    /// 内置插件没有句柄 ([dlHandle] 为空), 调用是空操作; 重复调用同样安全。
    void closeLibraryHandle() noexcept {
        if (!dlHandle) {
            return;
        }
        NativeLoader::close(dlHandle);
        dlHandle = nullptr;
    }

    /// stop 事务仍未执行: 同步关闭路径无法等待该事务，因此必须保留实例、
    /// 上下文与动态库，交由仍运行的异步 owner (unloadAsync/shutdownAsync) 收尾。
    /// - 加载成功的实例 start/stop 都在, `lifecycleStarted` 即"stop 欠着"的判据;
    /// - start 失败/未 start 的实例无需 stop (主程序回滚已声明注册), 可直接 destroy。
    bool lifecycleStopPending() const noexcept {
        return lifecycleStop != nullptr && lifecycleStarted && !lifecycleStopped;
    }

    /// 操作登记表 (句柄保活 / 完成端点 / 活动操作)
    ///
    /// 读写分处两条线程: 受理在调用线程 (插件线程), 收尾在 IO 线程, 因此一律在
    /// [recordsMutex] 下访问。
    mutable std::mutex                                                  recordsMutex;
    std::vector<std::shared_ptr<::PluginxxOperatorHandle>>              operatorHandles;
    std::vector<std::shared_ptr<::PluginxxOperationCompletionEndpoint>> completionEndpoints;
    std::vector<std::shared_ptr<::PluginxxOperatorHandle>>              outstandingOps;

    /// ==================== 通用表相关登记 (仅主程序 IO 线程读写) ====================
    ///
    /// 这些记录由通用表 (事件 / 调度 / 能力) 的实现维护, 与主程序业务无关, 因此直接
    /// 放在基类: 框架核心 (见 `pluginxx/host/host_core.h`) 只依赖基类即可完成
    /// 撤销与清理, 新增主程序无需重复实现同一套登记。

    /// 活跃广播订阅 (句柄本体由 [subscriptionHandles] 保活; 撤销时从此表移除)
    std::vector<std::shared_ptr<::PluginxxSubscription>> subscriptions;
    /// 广播订阅句柄保活表: 插件持有的裸指针在实例析构前始终有效
    std::vector<std::shared_ptr<::PluginxxSubscription>> subscriptionHandles;
    /// 活跃 sleep 使用 Operation 句柄索引；完成回调开始前移除，取消查询 O(1)。
    std::unordered_map<void*, std::shared_ptr<::PluginxxOperatorHandle>> sleepTimers;
    /// 已声明能力 (随工具注销/实例禁用卸载一并撤销)
    std::vector<PluginCapabilityRegistration> capabilityRegistrations;

    /// 驱动请求登记表 (`pluginxx.coroutine_runtime` 的 ticket 句柄)。
    ///
    /// 为什么需要这张表:
    /// - 插件桥接持有主程序发放的**裸指针**句柄, 它的有效性必须由主程序保证: 主程序
    ///   只把指针当**查表键**使用, 命中才解引用 (`shared_ptr` 保活), 因此即使
    ///   插件违约传入已收束/无效的句柄, 主程序也只是安全地忽略, 不会解引用悬垂内存;
    /// - 关闭超时需要"最后防线": 插件自身没能撤销的排队请求会一直持有实例 lease,
    ///   必须由主程序撤销 (见 [cancelPendingDrivers])。
    ///
    /// 内存有界: 只保留**未收束**请求 + 最近 [kFinishedDriverRetention] 张已收束
    /// 请求 (墓碑)。墓碑窗口保证"刚收束就取消"这类迟到 cancel 能按地址命中并
    /// 观察终态, 而不会命中"地址刚被回收给新请求"的旧句柄; 每次登记新请求时清理
    /// 超出窗口的墓碑, 因此长期运行不会无限增长。
    ///
    /// 线程: `request_driver`/`cancel_driver` 允许任意线程调用 (与其它注册表只由
    /// IO 线程访问不同), 因此本表用独立互斥保护。
    static constexpr size_t kFinishedDriverRetention = 16;

    mutable std::mutex                            driversMutex;
    std::deque<std::shared_ptr<::PluginxxDriver>> drivers;

    /// 登记请求句柄 (任意线程; 由 request_driver 在排队前调用)。
    void retainDriverHandle(const std::shared_ptr<::PluginxxDriver>& driver) {
        if (!driver) {
            return;
        }
        std::lock_guard lock(driversMutex);
        pruneFinishedDriversLocked();
        drivers.push_back(driver);
    }

    /// 按地址取消一次请求 (**任意线程可调用; 幂等**)。
    /// - `return`: true = 命中了登记表 (无论请求是否已收束, 都会调一次 cancel())
    /// - 未命中表示该句柄不属于本实例的有效窗口 (已收束且墓碑已过期, 或无效句柄):
    ///   安全忽略并记日志, 绝不解引用。
    bool cancelDriver(const ::PluginxxDriver* driver) noexcept {
        if (!driver) {
            return false;
        }
        std::shared_ptr<::PluginxxDriver> target;
        {
            std::lock_guard lock(driversMutex);
            for (const auto& entry : drivers) {
                if (entry.get() == driver) {
                    target = entry;
                    break;
                }
            }
        }
        if (!target) {
            XX_LOGW("Late plugin driver cancellation ignored (handle is not registered)");
            return false;
        }
        // 在锁外调用: cancel 可能触发请求收束后的收尾, 不能持表锁进入。
        target->cancel();
        return true;
    }

    /// 取消该实例全部**尚未开始**的请求 (关闭超时/最终收尾的安全网)。
    ///
    /// 正常关闭**不依赖**这里: 插件桥接在实例上下文销毁时自行 `cancel_driver`,
    /// 且 root 的取消收束依赖驱动继续流动 (见 plugin_driver.h 文件头)。主程序只在
    /// 已判定实例关闭失败 (关闭超时、lease 未归零) 时调用它, 避免 lease 永久残留。
    ///
    /// - `return`: 本次实际取消的请求数量 (0 表示没有排队中的请求)
    size_t cancelPendingDrivers() noexcept {
        std::vector<std::shared_ptr<::PluginxxDriver>> pending;
        {
            std::lock_guard lock(driversMutex);
            pending.reserve(drivers.size());
            for (const auto& driver : drivers) {
                if (driver && !driver->finished() && !driver->running()) {
                    pending.push_back(driver);
                }
            }
        }
        for (const auto& driver : pending) {
            driver->cancel();
        }
        return pending.size();
    }

    /// 尚未收束 (排队或执行中) 的请求数量, 供诊断与测试观察。
    size_t activeDriverCount() const noexcept {
        std::lock_guard lock(driversMutex);
        size_t          count = 0;
        for (const auto& driver : drivers) {
            if (driver && !driver->finished()) {
                ++count;
            }
        }
        return count;
    }

private:

    /// 从最旧一端清理已收束请求, 只保留最近 [kFinishedDriverRetention] 张作为墓碑。
    /// 调用方须持有 [driversMutex]。
    void pruneFinishedDriversLocked() {
        size_t finished = 0;
        for (const auto& driver : drivers) {
            if (!driver || driver->finished()) {
                ++finished;
            }
        }
        while (finished > kFinishedDriverRetention && !drivers.empty()) {
            const auto& oldest = drivers.front();
            if (oldest && !oldest->finished()) {
                // 未收束请求仍在排队/执行: 不能丢弃 (它持有实例 lease), 也不能
                // 越过它去回收后面的墓碑 (保持时间顺序, 避免误回收窗口内的句柄)。
                return;
            }
            drivers.pop_front();
            --finished;
        }
    }

public:

    /// 由实例创建路径设置，供只拿到裸指针的主程序回调升级 owner。
    std::weak_ptr<PluginInstanceBase> ownerSelf;

    /// 取实例自有的强引用
    /// - 供只拿到基类指针的主程序代码 (Operation 驱动器 / 完成回调) 升级为派生类型
    /// - `return` 空表示实例已析构，或创建路径未设置自引用
    template<typename InstanceT>
    std::shared_ptr<InstanceT> sharedSelf() const noexcept {
        if (auto base = ownerSelf.lock()) {
            return std::static_pointer_cast<InstanceT>(base);
        }
        return nullptr;
    }

    /// 框架生命周期控制块 (状态机 + 执行 lease)。实例对象本身只保存业务注册信息；
    /// 所有跨线程执行都通过 lease 保证 stop/destroy/dlclose 前已经返回。
    std::shared_ptr<pluginxx::InstanceLifetime> lifetime;

    /// 实例所属插件框架运行时 (io executor / Operation 表 / 投递通道)。
    ///
    /// 由管理器在加载 [lifetime] 时一并写入 (见 [PluginManagerBase::makeLifetime]);
    /// 弱引用是因为运行时由管理器持有且比实例长命, 实例不应延长其生命周期。
    /// Operation 驱动器与驱动请求据此拿到 io executor 与线程标识, 无需回查管理器
    /// 具体类型 (插件框架因此不依赖主程序的管理器类型)。
    std::weak_ptr<PluginRuntime> runtime;

    /// 框架控制块：交给插件的 `PluginxxHost` 视图保存在控制块内（进程级
    /// 稳定地址），插件在实例卸载后继续使用旧 host 指针时只会安全失败。
    /// 见 [PluginHostControl]。
    std::shared_ptr<PluginHostControl> hostControl;

    explicit PluginInstanceBase(std::string in_name) :
        name(std::move(in_name)) {}

    /// 析构时的最后一道保险: 插件上下文已经销毁 (或从未创建) 时, 句柄没被显式关闭
    /// 也在这里放掉 —— 否则插件文件会一直被进程占用到退出 (删不掉、换不掉)。
    /// 上下文还在时不关: 插件代码可能仍在运行, 关掉只会崩。
    virtual ~PluginInstanceBase() {
        if (pluginDestroyed || !pluginCreated) {
            closeLibraryHandle();
        }
    }

    PluginInstanceBase(const PluginInstanceBase&)            = delete;
    PluginInstanceBase& operator=(const PluginInstanceBase&) = delete;

    /// 执行 lease RAII: 把一段可能进入插件代码的执行登记到实例生命周期,
    /// 卸载路径的 `waitIdleUntil` 因此必然覆盖它, dlclose 不会越过仍在运行的插件代码。
    /// - `allowClosing=false` (默认): "开始新动作", 实例进入 Closing/Disabled 后获取失败;
    /// - `allowClosing=true`: 只读查询 / 取消 / 完成清理, 关闭过程中仍需执行。
    ///
    /// 未加载 lifetime 的实例 (单元测试直接构造的伪实例) 视为无租约约束。
    struct InflightGuard {
        PluginInstanceBase*                 inst = nullptr;
        std::shared_ptr<PluginInstanceBase> owner;
        pluginxx::InstanceLease             lease;

        explicit InflightGuard(std::shared_ptr<PluginInstanceBase> i, bool allowClosing = false) :
            inst(i.get()),
            owner(std::move(i)),
            lease(
                inst ? pluginxx::InstanceLease::acquire(inst->lifetime, allowClosing)
                     : pluginxx::InstanceLease{}
            ) {}

        explicit InflightGuard(PluginInstanceBase* i, bool allowClosing = false) :
            inst(i),
            owner(i ? i->ownerSelf.lock() : nullptr),
            lease(
                i ? pluginxx::InstanceLease::acquire(i->lifetime, allowClosing)
                  : pluginxx::InstanceLease{}
            ) {}

        explicit operator bool() const noexcept {
            return inst == nullptr || inst->lifetime == nullptr || static_cast<bool>(lease);
        }

        ~InflightGuard() = default;
    };

    /// 交给插件的主程序视图（控制块内地址，永不失效）；未加载控制块返回 nullptr。
    /// 插件保存该指针跨卸载继续调用时，各 vtable 入口会安全失败。
    const PluginxxHost* hostView() const noexcept;

    /// 插件上下文销毁后调用：旧 host 指针之后按“实例不存在”安全失败。
    void retireHostControl() noexcept;
};

// =====================================================================
// 插件代码派发（主程序 → 插件）
// =====================================================================

/// 一次派发的选项
struct PluginDispatchOptions {
    /// 关闭中的实例是否还要执行
    /// - false (默认): 常规回调 —— 实例进入 Closing 后不再进插件代码;
    /// - true: 生命周期收尾类 (stop / 取消 / 完成收尾) —— Closing 期间必须能跑,
    ///   `Closed` 仍拒绕 (拿不到 lease)。
    bool allowClosing = false;
    /// 插件执行器不可用 (缺失/已停止) 时是否保留动作, 绑定/恢复后重放
    /// - 常规回调传 false: 主程序没在执行, 这个回调已经没有意义;
    /// - 收尾类传 true: 不该因为线程刚好停了就丢掉。
    bool retainWhenStopped = false;
    /// 是否计入回调耗时统计 (只用于排查; 默认计)
    bool measure = true;
};

/// 把一段**插件代码**投递到该实例的执行器上执行
///
/// 这是“主程序调插件”的唯一入口。语义:
/// - **实例 lease 在派发时取**, 排队中的派发项也算在途: 实例关闭要等它跑完
///   (`waitInflightZero`), `dlclose` 不会越过已受理但尚未执行的回调;
/// - 同实例严格 FIFO (单执行器天然保证); 跨实例共享线程时各自 FIFO;
/// - 拿不到 lease (实例已在关闭/已禁用) 时直接丢弃并计入 `callbackStats.dropped`:
///   不抛异常、不回调, 调用方无需额外处理;
/// - 执行体抛出的异常只记日志 (跨 C ABI 抛异常属插件违约)。
///
/// - `return` false = 没有投递成功 (实例/运行时已失效、执行器不可用、拿不到 lease)。
/// 一次派发的载荷 (含 move-only 的执行 lease, 因此本体放在 shared_ptr 里: `std::function`
/// 要求可拷贝, 而 lambda 捕了 lease 就不可拷贝)
struct PluginDispatchContext {
    std::shared_ptr<PluginInstanceBase> inst;
    std::string                         label;
    std::function<void()>               fn;
    pluginxx::InstanceLease             lease;
    pluginxx::PluginCallbackStats*      stats = nullptr;
};

/// 记录一次"回调超过预算"并决定要不要告警 (**任意线程**)
///
/// 为什么限频: 这里量的是墙钟时间, 一次回调跑满 100 ms 有两种可能 —— 插件自己做了
/// 100 ms 的活, 或者它所在线程被系统调度挂起了这么久。后者与插件无关, 而只有周期很短的
/// 回调 (例如 5 秒采一次的监控广播) 才会统计到这种抖动, 于是同一插件会偶现同一条告警。
/// 判据改为"重复出现": 同一 (插件, 回调) 在窗口内第一次只记 DEBUG, 之后每个窗口最多
/// 一条 WARN, 带上窗口内的次数与最长耗时。计数 [PluginCallbackStats::overBudget] 仍每次
/// 自增, 诊断快照的 `over_budget_top` 能看到全貌。
inline void notePluginCallbackOverBudget(
    PluginCallbackStats& stats,
    std::string_view     pluginName,
    std::string_view     label,
    uint64_t             micros,
    uint64_t             budgetMs
) {
    constexpr auto   kWindow     = std::chrono::minutes{5};
    constexpr size_t kMaxRecords = 512;

    std::string key{pluginName};
    key += '|';
    key += label;

    uint64_t count     = 0;
    uint64_t maxMicros = 0;
    bool     warn      = false;
    {
        std::lock_guard lock(stats.overBudgetMutex);
        auto&           records = stats.overBudgetRecords;
        if (records.size() >= kMaxRecords && records.find(key) == records.end()) {
            // 记录表有上限: 满了先清掉上个窗口的项 (计数照旧, 只是不再新增键)
            const auto now = std::chrono::steady_clock::now();
            for (auto it = records.begin(); it != records.end();) {
                if (it->second.windowStart.time_since_epoch().count() != 0
                    && now - it->second.windowStart >= kWindow) {
                    it = records.erase(it);
                } else {
                    ++it;
                }
            }
        }
        auto&      record = records[key];
        const auto now    = std::chrono::steady_clock::now();
        if (record.windowStart.time_since_epoch().count() == 0
            || now - record.windowStart >= kWindow) {
            record.windowStart = now;
            record.count       = 0;
            record.maxMicros   = 0;
            record.lastWarn    = {};
        }
        ++record.count;
        record.maxMicros = (std::max)(record.maxMicros, micros);
        count            = record.count;
        maxMicros        = record.maxMicros;
        if (count > 1
            && (record.lastWarn.time_since_epoch().count() == 0
                || now - record.lastWarn >= kWindow)) {
            record.lastWarn = now;
            warn            = true;
        }
    }

    if (warn) {
        XX_LOGW(
            "Plugin `{}`: callback `{}` exceeded the {} ms budget {} time(s) in {} min "
            "(max {} ms); it keeps holding the plugin thread",
            pluginName,
            label,
            budgetMs,
            count,
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::minutes>(kWindow).count()
            ),
            maxMicros / 1000
        );
    } else if (count == 1) {
        // 单次超预算: 只可能是调度抖动 (插件本体耗时通常是指标里的平均值级别),
        // 记 DEBUG 供排查, 不打扰正常日志
        XX_LOGD(
            "Plugin `{}`: callback `{}` took {} ms once (budget {} ms), treated as a "
            "scheduling hiccup; repeated occurrences will be reported",
            pluginName,
            label,
            micros / 1000,
            budgetMs
        );
    }
}

/// 执行一次投递 (测量 + 异常保护 + 统计); 内联路径与插件线程路径共用
inline void runPluginDispatch(PluginDispatchContext& context) {
    const auto started = std::chrono::steady_clock::now();
    try {
        if (context.fn) {
            context.fn();
        }
    } catch (const std::exception& e) {
        XX_LOGE(
            "Plugin `{}`: callback `{}` threw: {}",
            context.inst->name,
            context.label,
            e.what()
        );
    } catch (...) {
        XX_LOGE(
            "Plugin `{}`: callback `{}` threw unknown exception",
            context.inst->name,
            context.label
        );
    }
    context.fn = {};
    // lease 提前释放: 后面的统计不需要再用锁住实例
    context.lease.reset();
    auto* stats = context.stats;
    if (!stats) {
        return;
    }
    const auto micros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started
        )
            .count()
    );
    stats->callbacks.fetch_add(1, std::memory_order_relaxed);
    stats->totalMicros.fetch_add(micros, std::memory_order_relaxed);
    uint64_t previousMax = stats->maxMicros.load(std::memory_order_relaxed);
    while (micros > previousMax
           && !stats->maxMicros.compare_exchange_weak(
               previousMax,
               micros,
               std::memory_order_relaxed
           )) {
    }
    const auto budget = static_cast<uint64_t>(stats->budgetMs.load(std::memory_order_relaxed));
    if (budget > 0 && micros > budget * 1000) {
        stats->overBudget.fetch_add(1, std::memory_order_relaxed);
        notePluginCallbackOverBudget(*stats, context.inst->name, context.label, micros, budget);
    }
}

inline bool dispatchToPlugin(
    const std::shared_ptr<PluginInstanceBase>& inst,
    std::string_view                           label,
    std::function<void()>                      fn,
    PluginDispatchOptions                      options = {}
) {
    if (!inst || !fn) {
        return false;
    }
    auto runtime = inst->runtime.lock();
    auto lease   = pluginxx::InstanceLease::acquire(inst->lifetime, options.allowClosing);
    if (inst->lifetime && !lease) {
        if (runtime && options.measure) {
            runtime->callbackStats.dropped.fetch_add(1, std::memory_order_relaxed);
        }
        XX_LOGD(
            "Plugin `{}`: callback `{}` dropped, instance is closing or closed",
            inst->name,
            label
        );
        return false;
    }
    auto context   = std::make_shared<PluginDispatchContext>();
    context->inst  = inst;
    context->label = std::string{label};
    context->fn    = std::move(fn);
    context->lease = std::move(lease);
    context->stats = (runtime && options.measure) ? &runtime->callbackStats : nullptr;

    // 没配独立插件执行器 (嵌入方程序 / 直接 new 管理器): 插件代码本来就在调用
    // 线程上跑, 就地执行即可 —— 与改造前完全一致, 不多一跳。
    if (!runtime || !pluginxx::hasPluginExecutor(runtime)) {
        runPluginDispatch(*context);
        return true;
    }

    const bool queued = pluginxx::enqueuePluginActionOn(
        runtime,
        pluginxx::pluginExecutionExecutor(runtime),
        [runtime, context] {
            runPluginDispatch(*context);
            (void)runtime;
        },
        options.retainWhenStopped
    );
    if (!queued) {
        if (options.measure) {
            runtime->callbackStats.unschedulable.fetch_add(1, std::memory_order_relaxed);
        }
        XX_LOGW(
            "Plugin `{}`: callback `{}` could not be scheduled (plugin executor is not "
            "available)",
            inst->name,
            label
        );
    }
    return queued;
}

// =====================================================================
// 框架控制块 (交给插件的 host 视图)
// =====================================================================

/// 框架控制块：插件持有的 `const PluginxxHost*` 必须指向进程级稳定地址。
///
/// 背景：插件在 create 时收到 host 指针，可能把它保存在实例字段、工作线程或
/// 延迟任务里；实例卸载（destroy + dlclose）之后插件仍可能调用主程序 vtable。
/// 若 host 视图位于 PluginInstance 对象内部，这类迟到调用就是 use-after-free。
///
/// 解决方式：
/// - 每个实例创建一块**永不释放**的控制块，host 视图放在其中，因此插件保存的
///   地址始终有效；实例关闭时只清空实例引用（tombstone）。
/// - `host.opaque` 是控制块地址，作为一次性令牌（地址永不复用），经进程级
///   注册表解析；已关闭实例的旧令牌解析成功但实例为空，所有入口安全失败，
///   既不会访问已释放对象，也不会把调用转交给后来加载的同名实例。
///
/// 控制块数量等于进程内累计加载过的插件实例数（每块约 100 字节）；这是保证
/// “旧 host 指针安全失败且绝不指向新实例”所付出的固定代价。
namespace detail {

/// 进程级控制块注册表。这里保存强引用的 tombstone 集合，不做回收：
/// 控制块必须比插件的引用更长命，地址才可能永不复用。
struct PluginHostControlRegistry {
    std::mutex                                                       mutex;
    std::map<void*, std::shared_ptr<PluginHostControl>, std::less<>> controls;
};

inline PluginHostControlRegistry& pluginHostControlRegistry() {
    static PluginHostControlRegistry registry;
    return registry;
}

} // namespace detail

class PluginHostControl {
public:

    /// 创建并注册控制块。`vtable` 为本端主程序静态函数表（各端各一份）。
    static std::shared_ptr<PluginHostControl> create(
        const std::shared_ptr<PluginInstanceBase>& instance,
        const PluginxxHostVtable*                  vtable
    ) {
        std::shared_ptr<PluginHostControl> control(new PluginHostControl(instance, vtable));
        registerControl(control);
        return control;
    }

    /// 交给插件的 host 视图地址（控制块内，永不失效）。
    const PluginxxHost* host() const noexcept {
        return &host_;
    }

    /// 一次性令牌（= 控制块地址，不复用）。
    void* token() const noexcept {
        return host_.opaque;
    }

    uint64_t generation() const noexcept {
        return generation_;
    }

    /// 实例仍在时返回强引用；已关闭/已释放返回空。
    std::shared_ptr<PluginInstanceBase> instance() const noexcept {
        return instance_.lock();
    }

    /// 实例关闭后调用：之后所有 vtable 入口按“实例不存在”安全失败。
    void retire() noexcept {
        retired_.store(true, std::memory_order_release);
        instance_.reset();
    }

    bool retired() const noexcept {
        return retired_.load(std::memory_order_acquire);
    }

private:

    PluginHostControl(
        const std::shared_ptr<PluginInstanceBase>& instance,
        const PluginxxHostVtable*                  vtable
    ) :
        instance_(instance),
        generation_(instance ? instance->lifetime ? instance->lifetime->generation() : 0 : 0) {
        host_.vtable = vtable;
        // 令牌即控制块地址：永不释放 => 永不复用，不会与后续实例混淆。
        host_.opaque = const_cast<PluginHostControl*>(this);
    }

    PluginHostControl(const PluginHostControl&)            = delete;
    PluginHostControl& operator=(const PluginHostControl&) = delete;

    /// 进程级注册表：保存控制块强引用的 tombstone 集合。
    /// 控制块必须比插件的引用更长命，因此这里不做回收。
    static void registerControl(const std::shared_ptr<PluginHostControl>& control) {
        if (!control) {
            return;
        }
        auto&           registry = detail::pluginHostControlRegistry();
        std::lock_guard lock(registry.mutex);
        registry.controls.emplace(control->token(), control);
    }

    PluginxxHost                      host_{};
    std::weak_ptr<PluginInstanceBase> instance_;
    uint64_t                          generation_ = 0;
    std::atomic<bool>                 retired_{false};
};

inline const PluginxxHost* PluginInstanceBase::hostView() const noexcept {
    return hostControl ? hostControl->host() : nullptr;
}

inline void PluginInstanceBase::retireHostControl() noexcept {
    if (hostControl) {
        hostControl->retire();
    }
}

/// C ABI 堆内存释放 (定义在本文件后半部分, 这里先声明: destroy 收尾要用到)
inline void hostMemoryFree(void* ptr);

namespace detail {

/// 投一次"destroy 完成后再关库"的收尾 (**不等结果**)
///
/// 用在无法 `co_await` 的同步路径 (owner 即将析构、进程退出前清场):
/// - 整段 (调 destroy → 退休上下文 → `dlclose`) 在插件/调用线程上跑, io 线程不等它;
/// - 派发项持有实例强引用, 因此实例 / 上下文 / DSO 都会等到 destroy 完成才释放;
/// - destroy 可能异步完成(插件在工作线程里 done), 所以完成端点用引用计数保活;
/// - `onFinished` 在完成时调一次(任意线程), 供调用方补状态。
///
/// - `return` false = 没能派发 (实例/运行时已失效), 调用方自行保留实例。
inline bool destroyInstanceNoWait(
    const std::shared_ptr<PluginInstanceBase>& inst,
    std::function<void()>                      onFinished = {}
) {
    if (!inst) {
        return false;
    }
    if (inst->pluginDestroyed) {
        inst->closeLibraryHandle();
        if (onFinished) {
            onFinished();
        }
        return true;
    }
    // 没有 destroy 入口 (从未建出上下文 / 主程序构造的伪实例): 只是本地收尾,
    // 不需要运行时与执行线程 —— 先判这一条, 否则缺运行时的实例会变成永远关不掉
    if (!inst->pluginCreated || !inst->lifecycleDestroy) {
        inst->finishDestroy();
        inst->closeLibraryHandle();
        if (onFinished) {
            onFinished();
        }
        return true;
    }
    auto runtime = inst->runtime.lock();
    if (!runtime || !inst->lifetime) {
        return false;
    }

    /// 完成端点: 插件可能稍后才 `done`, 因此这里只能有一份状态堆内存
    ///
    /// 关库必须等 destroy **入口返回**之后: 入口内部通常会 `notify->done(...)`,
    /// 那一刻栈上还有插件的返回地址, 在 `done` 里 `dlclose` 会让紧接的返回落到
    /// 已卸载的内存上 (实测 Windows 第二次确认时是 ``<Unloaded_xxx.dll>+偏移``)。
    struct DestroyOnce {
        std::shared_ptr<PluginInstanceBase> inst;
        std::function<void()>               onFinished;
        std::mutex                          mutex;
        bool                                doneCalled    = false; // 插件已交完成
        bool                                entryReturned = false; // destroy 入口已返回
        bool                                finished      = false; // 收尾已做过

        /// 两个条件都到齐才收尾 (关库 → 回调 → 自删); 由最后到场的调用方执行
        void maybeFinish() {
            bool run = false;
            {
                std::lock_guard lock(mutex);
                if (doneCalled && entryReturned && !finished) {
                    finished = true;
                    run      = true;
                }
            }
            if (!run) {
                return;
            }
            inst->finishDestroy();
            inst->closeLibraryHandle();
            if (onFinished) {
                onFinished();
            }
            delete this;
        }

        static void PLUGINXX_CALL
            onDone(void* ud, int32_t status, const PluginxxStringView* payload) noexcept {
            auto* self = static_cast<DestroyOnce*>(ud);
            if (!self) {
                return;
            }
            if (status != PLUGINXX_OPERATOR_OK && payload && payload->data) {
                XX_LOGW(
                    "Plugin `{}` destroy finished with failure: {}",
                    self->inst->name,
                    std::string(payload->data, static_cast<size_t>(payload->size))
                );
            }
            {
                std::lock_guard lock(self->mutex);
                if (self->doneCalled) {
                    return; // 重复 done: 忽略, 不重复收尾
                }
                self->doneCalled = true;
            }
            self->maybeFinish();
        }
    };

    auto* once      = new DestroyOnce{inst, std::move(onFinished), {}, false, false, false};
    auto  destroyFn = inst->lifecycleDestroy;
    auto  ctx       = inst->pluginCtx;
    const bool queued = dispatchToPlugin(
        inst,
        "plugin destroy",
        [destroyFn, ctx, once] {
            PluginxxOperatorNotify notify{&DestroyOnce::onDone, once};
            try {
                PluginxxString err{nullptr, 0};
                destroyFn(ctx, &notify, &err);
                if (err.data) {
                    hostMemoryFree(err.data);
                }
            } catch (const std::exception& e) {
                XX_LOGW("Plugin destroy threw: {}", e.what());
                DestroyOnce::onDone(once, PLUGINXX_OPERATOR_FAILED, nullptr);
            } catch (...) {
                XX_LOGW("Plugin destroy threw unknown exception");
                DestroyOnce::onDone(once, PLUGINXX_OPERATOR_FAILED, nullptr);
            }
            // destroy 入口已返回: 栈上不再有插件代码, 此时才允许关库
            // (`once` 到这里一定还在: `entryReturned` 只在这里置位, 所以 maybeFinish 还没跑过)
            {
                std::lock_guard lock(once->mutex);
                once->entryReturned = true;
            }
            once->maybeFinish();
        },
        PluginDispatchOptions{.allowClosing = true, .retainWhenStopped = true, .measure = false}
    );
    if (!queued) {
        delete once;
        return false;
    }
    return true;
}

} // namespace detail

/// 销毁完成后的收尾: 上下文作废 + 退休框架控制块 (之后插件持有的旧 host 指针只能安全失败)。
/// - 调 [destroyEntry] 之后由加载/卸载路径调用, 调用前须确认活动 lease 已归零;
/// - `pluginCreated == false` 也要调 (插件从未建出上下文, 只需退休控制块)。
inline void PluginInstanceBase::finishDestroy() noexcept {
    pluginCtx       = nullptr;
    pluginDestroyed = true;
    destroyDeferred = false;
    retireHostControl();
}
namespace detail {

/// 事件订阅撤销的簿记动作 (见 pluginxx/host/broadcast_registry.h 的声明)
/// - 调用方已把句柄的 `alive` 置 false, 这里只做注册表退订与实例订阅表清理;
/// - 幂等: `token` 置 0 后重复调用是空操作。
inline void revokeSubscription(PluginxxSubscription* sub) noexcept {
    if (!sub) {
        return;
    }
    try {
        if (sub->registry && sub->token != 0) {
            sub->registry->unsubscribe(sub->broadcastId, sub->token);
            sub->token = 0;
        }
    } catch (const std::exception& e) {
        XX_LOGW("Plugin broadcast unsubscribe failed: {}", e.what());
    } catch (...) {
        XX_LOGW("Plugin broadcast unsubscribe failed: unknown exception");
    }
    try {
        if (auto inst = sub->inst.lock()) {
            auto& subs = inst->subscriptions;
            subs.erase(
                std::remove_if(
                    subs.begin(),
                    subs.end(),
                    [sub](const std::shared_ptr<PluginxxSubscription>& entry) {
                        return entry.get() == sub;
                    }
                ),
                subs.end()
            );
        }
        sub->inst.reset();
    } catch (...) {
        XX_LOGW("Plugin subscription bookkeeping failed: unknown exception");
    }
}

/// 按频道 id 清掉实例订阅表里**已失效**的条目 (见 pluginxx/host/broadcast_registry.h)
/// - 用于“不等簿记完成”的撤销路径: 那条路径不持有句柄, 因此只能按频道 id 清理;
/// - 仍有效的条目 (同一频道重新订阅后的新句柄) 不动。
inline void
    forgetSubscription(PluginInstanceBase* inst, std::string_view broadcastId) noexcept {
    if (!inst || broadcastId.empty()) {
        return;
    }
    try {
        auto& subs = inst->subscriptions;
        subs.erase(
            std::remove_if(
                subs.begin(),
                subs.end(),
                [broadcastId](const std::shared_ptr<PluginxxSubscription>& entry) {
                    return entry && entry->broadcastId == broadcastId
                           && !entry->alive.load(std::memory_order_acquire);
                }
            ),
            subs.end()
        );
    } catch (...) {
        XX_LOGW("Plugin subscription bookkeeping failed: unknown exception");
    }
}

} // namespace detail

/// 解析插件传入的 host 视图对应的控制块。
/// - 未注册的令牌（含插件复制的 host 结构被篡改、旧内存被复用后的垃圾值）返回空；
/// - 已关闭实例返回控制块本身，调用方据此区分“实例不存在”与“参数非法”。
inline std::shared_ptr<PluginHostControl> resolvePluginHostControl(const PluginxxHost* host
) noexcept {
    if (!host || !host->opaque) {
        return nullptr;
    }
    try {
        auto&           registry = detail::pluginHostControlRegistry();
        std::lock_guard lock(registry.mutex);
        auto            it = registry.controls.find(host->opaque);
        return it == registry.controls.end() ? nullptr : it->second;
    } catch (...) {
        return nullptr;
    }
}

// =====================================================================
// C ABI 内存操作 + 主程序堆字符串构造 (跨 CRT 堆边界; 两侧 vtable 共用)
// =====================================================================

inline void* hostMemoryAlloc(uint64_t size) {
    return ::malloc(static_cast<size_t>(size));
}

inline void hostMemoryFree(void* ptr) {
    ::free(ptr);
}

inline PluginxxString hostMemoryCreateString(PluginxxStringView s) {
    PluginxxString res{nullptr, 0};
    if (!s.data && s.size == 0) {
        return res;
    }
    char* p = static_cast<char*>(hostMemoryAlloc(s.size + 1));
    if (p) {
        if (s.size > 0 && s.data) {
            std::memcpy(p, s.data, static_cast<size_t>(s.size));
        }
        p[s.size] = '\0';
        res.data  = p;
        res.size  = s.size;
    }
    return res;
}

inline PluginxxString hostMemoryCreateString(std::string_view sv) {
    return hostMemoryCreateString(PluginxxStringView{sv.data(), static_cast<uint64_t>(sv.size())});
}

inline void hostMemorySetString(PluginxxString* out, std::string_view sv) {
    if (!out) {
        return;
    }
    *out = hostMemoryCreateString(sv);
}

inline PluginxxString hostMemoryCreateString(const char* s) {
    if (!s) {
        return PluginxxString{nullptr, 0};
    }
    return hostMemoryCreateString(PluginxxStringView{s, static_cast<uint64_t>(std::strlen(s))});
}

// =====================================================================
// 可执行目录 helper (跨平台: Windows GetModuleFileNameW / Linux /proc/self/exe)
// 供 builtin:// 回退探测使用 (各端共用, 原两份实现合并)
// =====================================================================

inline std::filesystem::path getExecutableDirPath() noexcept {
#if XX_IS_WIN_D
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
#else
    std::error_code ec;
    auto            exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
    return exe.parent_path();
#endif
}

} // namespace pluginxx
