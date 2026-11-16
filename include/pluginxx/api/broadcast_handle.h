///
/// 归属: cxx_pluginxx (插件框架)。
///
/// 定位: 广播订阅句柄与"完成回调 `payload` 文本"之间的转换 (C++ 侧工具, header-only)。
///
/// 订阅走异步操作契约 (`subscribe_broadcast` / `subscribe_broadcast_ex`), 句柄只能经
/// 完成回调的 `payload` 交回。句柄本身由实例的订阅表保活到实例析构, 因此进程内传地址的
/// 十进制文本即可: 主程序用 [pluginxxSubscriptionToText] 生成, 插件侧原样转回
/// [pluginxxSubscriptionFromText] 再交给 `unsubscribe_broadcast`。
///
/// 主程序与插件共用同一份实现 (主程序实现表成员, 插件经 kit 转回句柄), 避免两侧各写一遍
/// 解析规则。
#pragma once

#include "pluginxx/api/abi.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pluginxx {

/// 订阅句柄 → 完成回调 `payload` 文本 (十进制地址)
inline std::string pluginxxSubscriptionToText(PluginxxSubscription* sub) {
    return std::to_string(reinterpret_cast<uintptr_t>(sub));
}

/// `payload` 文本 → 订阅句柄 (空 / 非法文本给 `nullptr`)
inline PluginxxSubscription* pluginxxSubscriptionFromText(std::string_view text) {
    if (text.empty()) {
        return nullptr;
    }
    uintptr_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return nullptr;
        }
        // 溢出按非法处理 (地址文本远短于此上限)
        if (value > (UINTPTR_MAX - 9) / 10) {
            return nullptr;
        }
        value = value * 10 + static_cast<uintptr_t>(c - '0');
    }
    return reinterpret_cast<PluginxxSubscription*>(value);
}

} // namespace pluginxx
