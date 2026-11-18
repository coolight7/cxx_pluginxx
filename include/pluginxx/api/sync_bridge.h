/// 同步桥回调 (插件 kit 与主程序的内部约定, **不属于 ABI**)
///
/// 背景: kit 的 `tableOpSyncWait` 会**阻塞调用方线程**直到表操作的完成回调到达。
/// 若主程序把这次回调投回插件执行器, 而阻塞的正是那条线程, 就成了自己等自己
/// (完成回调排在已阻塞的派发项后面, 永远轮不到)。
///
/// 做法: 同步等待把回调包成 [SyncBridgeCallback] (固定标记 + 真正的唤醒体 + 它的 ud),
/// 主程序识别标记后**在完成线程上就地调用** —— 完成线程可能是 io 线程 (表操作在 io
/// 上执行), 也可能是调用方自己 (调用方本来就在 io 时是内联完成)。
///
/// 安全性: 就地调用的只有 kit 的唤醒体 (往共享结构里写状态 + 通知条件变量), 不碰插件
/// 自己的业务状态; 插件的业务代码仍在插件执行线程上继续执行。因此"插件执行线程同步
/// 等 io"是合法方向 —— 卡住的只是插件自己, io 线程从不阻塞等插件线程。
///
/// 归属: 本头是 kit (`pluginxx/kit/kit.h`) 与主程序 (`pluginxx/runtime/op_driver.h`)
/// 之间的约定, 同一份 cxx_pluginxx 内编译, 不跨动态库边界。
#pragma once

#include "pluginxx/api/abi.h"

#include <cstdint>
#include <memory>

namespace pluginxx {

/// 标记值 ("pluginxx sync"), 用来把同步桥与普通完成回调区分开
inline constexpr uint64_t kSyncBridgeMagic = 0x636e79737878706cULL;

/// 一次同步等待的包装回调 (kit 堆分配, 由 [invokeSyncBridge] 释放)
struct SyncBridgeCallback {
    uint64_t                 magic = kSyncBridgeMagic;
    /// 真正的唤醒体 (只唤醒等待者, 不跑插件业务)
    PluginxxOperatorCallback fn = nullptr;
    /// 唤醒体的上下文
    void* ud = nullptr;
};

/// 同步桥统一入口: 交给表成员的 `cb`
///
/// 释放包装块后再调唤醒体 (唤醒体自己管理它的 `ud`)。
inline void PLUGINXX_CALL
    invokeSyncBridge(void* ud, int32_t status, const PluginxxStringView* payload) {
    auto* bridge = static_cast<SyncBridgeCallback*>(ud);
    if (bridge == nullptr) {
        return;
    }
    std::unique_ptr<SyncBridgeCallback> keep(bridge);
    if (bridge->fn != nullptr) {
        bridge->fn(bridge->ud, status, payload);
    }
}

/// 这个完成回调是不是同步桥 (主程序侧判定: 是则就地调用, 不投递)
///
/// `ud` 在任何情况下都是调用方给的上下文; 非空时至少指向一个指针大小的有效对象
/// (随后会被回调本身解引用), 因此读标记是安全的。
inline bool isSyncBridgeCallback(const void* ud) noexcept {
    return ud != nullptr && static_cast<const SyncBridgeCallback*>(ud)->magic == kSyncBridgeMagic;
}

/// 同步等待在"插件执行线程"上的准入策略
enum class SyncWaitThreadPolicy {
    /// 默认: 插件执行线程上拒绝 (agent 侧跨插件调用可能把工作体派发到同一条线程)
    refuseOnPluginThread,
    /// 主程序保证工作体不会被派发回本插件的执行线程时使用 (client 表: 只碰 client 注册表)
    allowOnPluginThread,
};

} // namespace pluginxx
