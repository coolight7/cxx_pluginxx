/// 能力调用的通用工具 (阶段链顺序 + JSON 补丁合并)
///
/// 归属: cxx_pluginxx (插件框架)。这些函数与主程序业务无关:
/// - `appendStage`: 把某个阶段的实现行按执行顺序取出来 (before / impl 升序, after 倒序);
/// - `mergeJsonPatch`: 实现行返回的补丁深合并 (对象逐键递归, 其它类型替换);
/// - `parseJson`: 容错解析 (失败返回 ok=false, 不抛异常)。
///
/// 线程约定: 全部是纯函数, 不持有状态; 这里不碰注册表的内部锁。
#pragma once

#include "pluginxx/host/capability_registry.h"
#include "utilxx_base/json.h"

#include <string>
#include <string_view>
#include <vector>

namespace pluginxx {
namespace detail {

/// JSON 解析结果 (失败时 ok = false)
struct ParsedJson {
    bool              ok = false;
    utilxx_base::Json value;
};

/// 容错解析: 空串与坏 JSON 都返回 ok = false, 不抛异常
inline ParsedJson parseJson(std::string_view text) {
    ParsedJson out;
    if (text.empty()) {
        return out;
    }
    try {
        out.value = utilxx_base::Json::parse(text);
        out.ok    = true;
    } catch (...) {
        out.ok = false;
    }
    return out;
}

/// 深合并补丁: 对象逐键递归合并, 其它类型 (数组、标量、null) 直接替换
inline void mergeJsonPatch(utilxx_base::Json& target, const utilxx_base::Json& patch) {
    if (!patch.is_object()) {
        target = patch;
        return;
    }
    if (!target.is_object()) {
        target = utilxx_base::Json::object();
    }
    for (const auto& [key, value] : patch.items()) {
        if (target.contains(key) && value.is_object() && target[key].is_object()) {
            mergeJsonPatch(target[key], value);
        } else {
            target[key] = value;
        }
    }
}

/// 把某个阶段的行按执行顺序追加到 `out` (after 倒序; 同一阶段内按 priority、登记顺序)
///
/// `scopeId` 非空时只取对它有作用的行 (空作用域的行 + 同作用域的行); 空 = 不过滤。
inline void appendStage(
    std::vector<CapabilityProvider>& out,
    const CapabilityRegistry&        registry,
    std::string_view                 id,
    CapabilityPhase_e                phase,
    bool                             reverse,
    std::string_view                 scopeId = {}
) {
    auto rows = registry.rowsOf(id, phase, scopeId);
    if (reverse) {
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            out.push_back(**it);
        }
        return;
    }
    for (const auto* row : rows) {
        out.push_back(*row);
    }
}

} // namespace detail
} // namespace pluginxx
