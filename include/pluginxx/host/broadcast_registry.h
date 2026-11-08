/// 插件广播注册表 (主程序侧, 与主程序业务无关)
///
/// 归属: cxx_pluginxx (插件框架)。
///
/// 定位: 广播是"属主改了东西, 通知所有关心的人":
/// - 声明 ([BroadcastDecl]): 这个频道有没有当前值 (`cache`)、谁能写 (`writable`)、
///   订阅时是否立刻收到当前值 (`pushOnSubscribe`)、允许合并 (`coalesce`)、
///   是不是调试频道 (`scope`);
/// - 频道 ([BroadcastChannel]): 一份缓存 + 订阅者。
///
/// 三条语义:
/// 1. `cache: latest` 只保证"最后一次到达": 同一频道重复发布时缓存只留最后一份,
///    订阅方拿到的是最新值 (中间值不再补发); `cache: none` 没有当前值, 也不能 `read`。
/// 2. **广播的缓存不等于能力的值缓存**: 这里是"这个频道现在是什么值"(一份),
///    能力的 `byIdentity` 是"这个请求身份的结果"(多份), 两者分开存放。
/// 3. 订阅互不等待: 一个订阅抛异常只记日志, 不影响属主与其它订阅。
///
/// 线程约定: 订阅登记与发布都可能来自不同线程 (IO 线程 / 事件线程), 内部用互斥保护
/// 频道表; 回调一律在锁外执行, 因此订阅方可以在回调里再次发布或退订。
#pragma once

#include "pluginxx/api/abi.h"
#include "pluginxx/export.h"
#include "pluginxx/runtime/runtime.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pluginxx {

/// 插件实例基类 (定义在 `pluginxx/runtime/instance_base.h`; 这里只需要弱引用)
class PluginInstanceBase;

/// 频道有没有当前值 (`PLUGINXX_BROADCAST_CACHE_*`)
enum class BroadcastCache_e : int32_t {
    None   = 0,
    Latest = 1,
};

/// 谁能写 (`PLUGINXX_BROADCAST_WRITABLE_*`)
enum class BroadcastWritable_e : int32_t {
    Owner  = 0, ///< 只有声明的属主能写 (状态镜像)
    Anyone = 1, ///< 任何已连接的插件都能写 (变量)
};

/// 频道用途 (`PLUGINXX_BROADCAST_SCOPE_*`)
enum class BroadcastScope_e : int32_t {
    App    = 0,
    System = 1, ///< 调试 / 自省频道: 插件不能写
};

/// 广播声明
struct BroadcastDecl {
    std::string           id;
    BroadcastCache_e      cache           = BroadcastCache_e::None;
    BroadcastWritable_e   writable        = BroadcastWritable_e::Owner;
    bool                  pushOnSubscribe = false;
    bool                  coalesce        = true;
    BroadcastScope_e      scope           = BroadcastScope_e::App;
    /// 属主: 空 = 应用 (Dart 下发); 非空 = 插件名 (插件自建频道)
    std::string           owner;

    bool isAppChannel() const noexcept {
        return owner.empty();
    }
};

/// 一个订阅
struct BroadcastSub {
    uint64_t                              token = 0;
    std::string                           owner; ///< 订阅方 (插件名; 空 = 宿主)
    std::function<void(std::string_view)> handler;
};

/// 一个频道: 声明 + 当前值 + 订阅者
struct BroadcastChannel {
    BroadcastDecl           decl;
    std::string             latest;    ///< 最后一个值 (cache: latest)
    bool                    hasLatest = false;
    std::string             lastWriter; ///< 最近一次写入的属主 (自省用)
    uint64_t                nextToken = 0;
    std::vector<BroadcastSub> subs;
};

/// 广播注册表: id → 频道
class PLUGINXX_API BroadcastRegistry {
public:

    /// 声明一个频道
    /// - 重复声明: 应用声明不能被插件覆盖; 同为插件声明时后声明者覆盖;
    /// - 收窄成 `cache: none` 时丢掉缓存的当前值;
    /// - 校验失败写 `error` (可为空) 并返回 false
    bool declare(const BroadcastDecl& decl, std::string* error = nullptr);

    bool declares(std::string_view id) const;

    /// 声明 (不存在返回 nullptr; 指针在下次声明变化前有效)
    const BroadcastDecl* declOf(std::string_view id) const;

    /// 发布
    /// - 未声明的 id、`scope: system` 的插件写、`writable: owner` 的非属主写都拒绝,
    ///   并把原因写进 `error` (可为空);
    /// - 成功时缓存与投递都在本次调用内完成 (同步)。
    bool publish(
        std::string_view id,
        std::string_view payload,
        std::string_view writer,
        std::string*     error = nullptr
    );

    /// 读当前值: 只有 `cache: latest` 且确实有过值才返回 true
    bool read(std::string_view id, std::string* out) const;

    /// 订阅
    /// - 返回订阅令牌 (0 = 失败); `pushOnSubscribe` 且已有当前值时,
    ///   在返回前用当前值回调一次 (锁外);
    /// - `owner` 只用于按属主批量退订与自省。
    uint64_t subscribe(
        std::string_view                      id,
        std::string_view                      owner,
        std::function<void(std::string_view)> handler,
        std::string*                          error = nullptr
    );

    /// 退订 (幂等); 命中返回 true
    bool unsubscribe(std::string_view id, uint64_t token);

    /// 退订某个订阅方的全部订阅 (插件卸载/禁用); 返回撤销数量
    size_t removeSubscriber(std::string_view owner);

    /// 删除一个频道 (变量注销 / 宿主清理); 命中返回 true
    bool removeChannel(std::string_view id);

    /// 删除某个插件的频道并撤销其订阅 (插件卸载); 返回删除的频道数
    size_t removeChannelsOwnedBy(std::string_view owner);

    /// 全部已声明的 id (字典序)
    std::vector<std::string> ids() const;

    /// 自省: 一个频道 (JSON); 未声明返回空串
    std::string infoJson(std::string_view id) const;

    /// 自省: 全部频道 (JSON)
    std::string listJson() const;

    /// 清空 (单元测试用)
    void reset();

    /// 声明校验; 成功返回空串
    static std::string validateDecl(const BroadcastDecl& decl);

private:

    /// 投递 (锁外); 单个回调抛异常只记日志
    static void deliver(
        const std::vector<BroadcastSub>& subs,
        std::string_view                 payload,
        std::string_view                 id
    );

    mutable std::mutex                                          mutex_;
    std::map<std::string, BroadcastChannel, std::less<>>        channels_;
};

} // namespace pluginxx

/* ==================== 订阅句柄 (C ABI 不透明指针的实现体) ==================== */

/// 广播订阅句柄的**实现体** (C ABI 中作为不透明指针 `PluginxxSubscription*` 传递,
/// 因此名字保持 ABI 冻结时的形态)。
///
/// 生命周期:
/// - 句柄本体由插件实例的 `subscriptionHandles` 保活, 插件持有的裸指针在实例析构前始终有效;
/// - `alive` 为原子标志: 撤销返回后发布线程据此立即短路, 不再回调插件;
/// - 回调本身经实例执行 lease 保护 (见 [pluginxx::PluginHostCore::subscribeBroadcast])。
struct PluginxxSubscription {
    /// 广播注册表 (句柄持有, 保证撤销动作执行时后端仍然有效)
    std::shared_ptr<pluginxx::BroadcastRegistry> registry;
    /// 频道 id (已补齐命名空间的最终形式)
    std::string broadcastId;
    /// 注册表里的订阅令牌 (0 = 未登记或已撤销)
    uint64_t token = 0;
    /// 插件实例 (弱引用: 句柄不延长实例生命周期)
    std::weak_ptr<pluginxx::PluginInstanceBase> inst;
    /// 订阅时的作用域标识 (空 = 订阅的是主程序总线; 卸载作用域时据此筛出该作用域的订阅)
    std::string scopeId;
    /// 插件框架运行时 (撤销时把簿记投递到 IO 线程; 弱引用不延长其生命周期)
    std::weak_ptr<pluginxx::PluginRuntime> runtime;
    /// 插件广播回调 (C ABI 函数指针) 与它的上下文
    void(PLUGINXX_CALL* handler)(const PluginxxStringView* payload_json, void* ud) = nullptr;
    void* ud = nullptr;
    /// 是否仍然有效: 撤销时先置 false, 发布线程据此立即短路
    std::atomic<bool> alive{true};
};

namespace pluginxx {
namespace detail {

/// 撤销簿记的具体动作 (**须在主程序 IO 线程执行**): 撤销注册表订阅并移出实例订阅表。
/// - 幂等: `token` 置 0 后重复调用是空操作;
/// - 异常不外抛 (撤销发生在实例关闭路径, 不能因为后端报错而留下悬挂登记)。
///
/// 定义放在 `pluginxx/runtime/instance_base.h` (那里 [PluginInstanceBase] 已完整):
/// 本头只能前置声明实例类型, 不能在这里访问其成员。
void revokeSubscription(PluginxxSubscription* sub) noexcept;

} // namespace detail

/// 撤销广播订阅 (**幂等**, 任意线程可调用)
///
/// 步骤:
/// 1. 原子置 `alive=false`: 之后到达的发布立即不再回调插件, 不必等待簿记完成;
/// 2. 在主程序 IO 线程撤销注册表订阅并移出实例的订阅表 (非 IO 线程时同步等待投递完成);
/// 3. 运行时不可投递 (io executor 缺失/已停止) 时就地完成簿记 —— 此时实例正在
///    关闭, 不再有并发访问实例订阅表的执行体。
///
/// 句柄本体的保活由实例的 `subscriptionHandles` 负责 (随实例析构释放), 因此插件在
/// 撤销之后再传同一裸指针进来也只会命中 [detail::revokeSubscription] 的空操作分支。
inline void unsubscribePluginBroadcast(PluginxxSubscription* sub) {
    if (!sub) {
        return;
    }
    bool expected = true;
    if (!sub->alive.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return; // 已撤销: 幂等空操作
    }

    auto runtime = sub->runtime.lock();
    if (!runtime || isRuntimeIoThread(runtime) || runtimeExecutorStopped(runtime->executor)) {
        detail::revokeSubscription(sub);
        return;
    }

    auto       done   = std::make_shared<std::promise<void>>();
    auto       fut    = done->get_future();
    const bool queued = enqueueRuntimeAction(
        runtime,
        [runtime, sub, done]() {
            runtime->ioThreadId.store(std::this_thread::get_id(), std::memory_order_release);
            detail::revokeSubscription(sub);
            done->set_value();
        },
        false
    );
    if (!queued) {
        detail::revokeSubscription(sub);
        return;
    }
    fut.wait();
}

} // namespace pluginxx
