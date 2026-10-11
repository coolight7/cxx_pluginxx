/// pluginxx 接口表元信息 (每张表的"最低兼容版本 + 基线字节数", 以及成员可用性判定)
///
/// 为什么需要它: 表头只有 IID / `version` / `struct_size` 三个常量时, 判定只能写死
/// 字面量 (例如 `iface->version != 1`), 于是**任何一张表升版都会让所有表的查询一起
/// 失败**; 追加成员时也看不出"对方没有这个成员"。本头把判定需要的信息集中成一处:
///
/// - [IfaceMeta]: 每张接口表一项 (最低兼容版本, 基线字节数), 由声明该表的头提供特化;
/// - [ifaceHas]: 对方给的表长是否覆盖某个成员 (追加成员必须用它判定, 只判空指针不够);
/// - [ifaceHasVersion]: 语义档位判定 (布局不变、含义变化时用)。
///
/// 每张表四件套齐全 (缺一不可, 测试模块 `boundaries` 逐表校验):
/// IID 宏 + `struct_size` + `version` (`X_VERSION`) + `MIN_VERSION` + `BASELINE_SIZE`。
///
/// 整表判定规则 (见 `pluginxx/kit/kit.h` 的 [pluginxx::validateInterface]):
/// `version >= MIN_VERSION` 且 `struct_size >= BASELINE_SIZE` → 表可用。
///
/// 表可用不等于全部成员可用: 基线以内的成员由整表门槛保证, **追加段成员**必须
/// `ifaceHas` 判定 (见下)。此处的"追加"指表尾追加成员, 见
/// docs/zh-cn/design/plugins.md §9 的追加策略。
#ifndef PLUGINXX_API_IFACE_META_H
#define PLUGINXX_API_IFACE_META_H

#include "pluginxx/api/tables.h"

#include <cstddef>
#include <cstdint>

namespace pluginxx {

/// 接口表元信息: 最低兼容版本 + 基线字节数
///
/// 未特化的类型按"必须完整"处理 (最低版本 1, 基线 = 完整表长): 新增表忘记补特化时
/// 行为退化为"整表必须齐全", 不会放宽判定。
template<typename Iface>
struct IfaceMeta {
    static constexpr int32_t  min_version   = 1;
    static constexpr uint32_t baseline_size = static_cast<uint32_t>(sizeof(Iface));
};

/// 取某个成员的结束偏移 (成员偏移 + 成员大小)
///
/// 实现说明: `offsetof` 只接受成员名字, 拿不到"成员指针"这类模板参数, 因此这里用
/// 一个值初始化的探针对象取两个地址相减。不在常量表达式里求值 (取地址不是常量表达式),
/// 调用点无需关心; 每次只构造一个零初始化的表对象 (几十到几百字节), 调用频率极低。
template<typename Spec, typename M>
inline uint32_t memberEndOf(M Spec::* member) noexcept {
    const Spec  probe{};
    const char* base  = reinterpret_cast<const char*>(&probe);
    const char* field = reinterpret_cast<const char*>(&(probe.*member));
    return static_cast<uint32_t>(field - base) + static_cast<uint32_t>(sizeof(probe.*member));
}

/// [memberEndOf] 的模板参数形态 (`Spec` 由调用点显式给出)
template<auto Member, typename Spec>
inline uint32_t memberEndOffset() noexcept {
    return memberEndOf(Member);
}

/// 追加成员可用性: 对方给的表长是否覆盖该成员
///
/// - **必须与判空同时满足**: 整表门槛放宽到基线后, 越过对方表尾去读成员读到的是
///   紧邻的另一块内存, 几乎不可能是 NULL, 只判空会调进野地址;
/// - 基线以内的成员不需要它 (整表门槛已保证), 判定成本只落在追加段成员上;
/// - 用法: `ifaceHas<&PluginxxLogIface::log>(logIface) && logIface->log`。
///
/// - `args`:
///     - [t] 已通过整表判定的接口表 (可为 NULL, 返回 false)
///
/// - `return` 表长覆盖该成员返回 true
template<auto Member, typename Iface>
inline bool ifaceHas(const Iface* t) noexcept {
    return t != nullptr && t->struct_size >= memberEndOffset<Member, Iface>();
}

/// 语义档位判定: 表声明的版本是否达到 `v`
///
/// 用于"布局没变、含义变了"的改动 (例如某个成员 v2 起返回值语义不同)。追加成员用
/// [ifaceHas], 不要用本函数。
template<typename Iface>
constexpr bool ifaceHasVersion(const Iface* t, int32_t v) noexcept {
    return t != nullptr && t->version >= v;
}

/* ==================== 通用表元信息特化 (每张表一项) ==================== */

template<>
struct IfaceMeta<PluginxxEventsIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_EVENTS_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_EVENTS_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_EVENTS_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_EVENTS_BASELINE_SIZE <= sizeof(PluginxxEventsIface));

template<>
struct IfaceMeta<PluginxxCapabilitiesIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_CAPABILITIES_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_CAPABILITIES_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_CAPABILITIES_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_CAPABILITIES_BASELINE_SIZE <= sizeof(PluginxxCapabilitiesIface));

template<>
struct IfaceMeta<PluginxxSchedulerIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_SCHEDULER_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_SCHEDULER_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_SCHEDULER_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_SCHEDULER_BASELINE_SIZE <= sizeof(PluginxxSchedulerIface));

template<>
struct IfaceMeta<PluginxxCoroutineRuntimeIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_COROUTINE_RUNTIME_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_COROUTINE_RUNTIME_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_COROUTINE_RUNTIME_BASELINE_SIZE >= 8u);
static_assert(
    PLUGINXX_IFACE_COROUTINE_RUNTIME_BASELINE_SIZE <= sizeof(PluginxxCoroutineRuntimeIface)
);

template<>
struct IfaceMeta<PluginxxPluginsIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_PLUGINS_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_PLUGINS_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_PLUGINS_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_PLUGINS_BASELINE_SIZE <= sizeof(PluginxxPluginsIface));

template<>
struct IfaceMeta<PluginxxConfigIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_CONFIG_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_CONFIG_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_CONFIG_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_CONFIG_BASELINE_SIZE <= sizeof(PluginxxConfigIface));

template<>
struct IfaceMeta<PluginxxCancelIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_CANCEL_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_CANCEL_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_CANCEL_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_CANCEL_BASELINE_SIZE <= sizeof(PluginxxCancelIface));

template<>
struct IfaceMeta<PluginxxJsonIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_JSON_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_JSON_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_JSON_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_JSON_BASELINE_SIZE <= sizeof(PluginxxJsonIface));

template<>
struct IfaceMeta<PluginxxLogIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_LOG_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_LOG_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_LOG_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_LOG_BASELINE_SIZE <= sizeof(PluginxxLogIface));

template<>
struct IfaceMeta<PluginxxTasksIface> {
    static constexpr int32_t  min_version   = PLUGINXX_IFACE_TASKS_MIN_VERSION;
    static constexpr uint32_t baseline_size = PLUGINXX_IFACE_TASKS_BASELINE_SIZE;
};
static_assert(PLUGINXX_IFACE_TASKS_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_TASKS_BASELINE_SIZE <= sizeof(PluginxxTasksIface));

} // namespace pluginxx

#endif /* PLUGINXX_API_IFACE_META_H */
