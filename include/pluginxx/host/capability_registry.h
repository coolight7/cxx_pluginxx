/// 插件能力注册表 (主程序侧, 与主程序业务无关)
///
/// 归属: cxx_pluginxx (插件框架)。
///
/// 定位: 能力的**声明册 + 实现行**:
/// - 声明 ([CapabilityDecl]): 这个 id 允许几个实现 (`impls`)、结果怎么合并 (`merge`)、
///   开放哪些阶段 (`phases`)、最多等多久 (`timeoutMs`)、是谁声明的 (`declaredBy`);
/// - 实现行 ([CapabilityProvider]): 谁 (`provider` / `ownerTag`) 在哪个阶段、什么优先级、
///   同步还是异步、启动回调、取消回调与上下文。
///
/// 覆盖规则 (后登记的覆盖先登记的):
/// - `impls: one` 的 **impl 行**只有一个: 跨插件也覆盖, 并记一条日志写清谁覆盖了谁;
///   `before` / `after` 是干预链, 多个插件可以各登记一行, 只有同 `provider` +
///   `ownerTag` + 阶段重复登记才覆盖 (否则"只让插件改输入/输出"的埋点就只能有一个插件用)。
/// - `impls: many` 的所有阶段都可多行共存, 只有同 `provider` + `ownerTag` + 阶段重复登记才覆盖。
/// - 归属跟着新登记行走: 卸载时按行里的 `provider` 筛, 不会删掉覆盖者的行。
///
/// 线程约定: 仅主程序 IO 线程读写 (与插件注册事务同一串行上下文), 因此内部不加锁。
///
/// - 相关: 调用与插件归属校验见 `PluginHostCore::invokeCapabilityAsync`
#pragma once

#include "pluginxx/api/tables.h"
#include "pluginxx/export.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pluginxx {

/// 实例侧的能力登记记录 (提供者插件自己的登记表, 卸载/禁用时据此逐条撤销)
///
/// 说明: 这是 `PluginInstanceBase` 的字段类型, 与注册表内的实现行是两份数据 ——
/// 实例只记"我登记过哪些 id", 真正的阶段 / 优先级 / 回调在 [CapabilityProvider] 里。
struct PluginCapabilityRegistration {
    std::string                     name;   ///< 能力名
    PluginxxCapabilityStartFunction start  = nullptr;
    PluginxxOperatorCancelFunction  cancel = nullptr;
    void*                           ctx    = nullptr;
};

/// 一个 id 上有几个实现 (`PLUGINXX_CAPABILITY_IMPLS_*`)
enum class CapabilityImpls_e : int32_t {
    One  = 0,
    Many = 1,
};

/// 实现链的结果怎么合并 (`PLUGINXX_CAPABILITY_MERGE_*`)
enum class CapabilityMerge_e : int32_t {
    None  = 0, ///< 不适用 (单实现, 或不开放 impl)
    First = 1, ///< 第一个给出合法结果的生效
    Veto  = 2, ///< 任一实现给出取消即否
};

/// 一次登记落在哪个阶段 (`PLUGINXX_CAPABILITY_PHASE_*`, 互斥的一个值)
enum class CapabilityPhase_e : int32_t {
    Impl   = 1,
    Before = 2,
    After  = 4,
};

/// 阶段位 (声明里的 `phases`)
inline constexpr uint32_t kCapabilityPhaseMaskImpl   = 1u << 0;
inline constexpr uint32_t kCapabilityPhaseMaskBefore = 1u << 1;
inline constexpr uint32_t kCapabilityPhaseMaskAfter  = 1u << 2;

/// 能力声明
struct CapabilityDecl {
    std::string       id;
    CapabilityImpls_e impls     = CapabilityImpls_e::One;
    CapabilityMerge_e merge     = CapabilityMerge_e::None;  ///< impls=many 且含 impl 时才看
    uint32_t          phases    = kCapabilityPhaseMaskImpl; ///< before | impl | after 的子集
    int32_t           timeoutMs = 0;                        ///< 0 = 不设上限, 由调用方决定
    /// 声明方: 空 = 应用 (Dart 下发); 非空 = 插件名 (插件自声明的能力)
    std::string declaredBy;

    bool isAppDeclaration() const noexcept {
        return declaredBy.empty();
    }
    bool openImpl() const noexcept {
        return (phases & kCapabilityPhaseMaskImpl) != 0;
    }
    bool openPhase(CapabilityPhase_e phase) const noexcept {
        return (phases & static_cast<uint32_t>(phase)) != 0;
    }
};

/// 一条实现行
struct CapabilityProvider {
    std::string                     provider; ///< 插件实例名 (与 manifest 的 name 一致)
    std::string                     ownerTag; ///< 同一插件内区分多个处理器 (可空)
    CapabilityPhase_e               phase    = CapabilityPhase_e::Impl;
    int32_t                         priority = 0; ///< 小者先
    bool                            sync     = false;
    PluginxxCapabilityStartFunction start    = nullptr;
    PluginxxOperatorCancelFunction  cancel   = nullptr;
    void*                           ctx      = nullptr;
    uint64_t                        seq      = 0; ///< 登记顺序 (同优先级时保持稳定)
};

/// 首个 impl 行的视图 (兼容旧的"一个能力一个提供者"调用方)
struct CapabilityEntry {
    std::string                     provider;
    PluginxxCapabilityStartFunction start  = nullptr;
    PluginxxOperatorCancelFunction  cancel = nullptr;
    void*                           ctx    = nullptr;
};

/// 一个 id 的现状: 声明 + 实现行 (rows 按 (阶段, priority, 登记顺序) 排好)
struct CapabilitySlot {
    CapabilityDecl                  decl;
    std::vector<CapabilityProvider> rows;
    /// 首个 impl 行的缓存视图: `get()` 返回它的地址, 指针在下次增删行之前保持有效
    CapabilityEntry first;
    /// 下一个行的登记序号 (同优先级时保持登记顺序)
    uint64_t nextSeq = 1;
};

/// 能力注册表: id → (声明 + 实现行)
class PLUGINXX_API CapabilityRegistry {
public:

    /// 旧的单提供者视图 (兼容 `get` 的返回值): 首个 impl 行
    using Entry = CapabilityEntry;

    /// 声明一条能力
    /// - 重复声明: 应用声明覆盖插件声明 (应用是权威来源), 同为插件声明时后声明者覆盖;
    /// - 校验失败写 `error` (可为空) 并返回 false
    bool declare(const CapabilityDecl& decl, std::string* error = nullptr);

    /// 是否已声明 (含没有实现行的声明)
    bool declares(std::string_view id) const;

    /// 追加一条实现行
    /// - 必须已声明, 且该阶段在声明里开放;
    /// - `impls: one` 的 impl 行覆盖已有 impl 行 (记日志); 其它情况按
    ///   (provider, ownerTag, 阶段) 覆盖;
    /// - 校验失败写 `error` (可为空) 并返回 false
    bool addProvider(std::string_view id, const CapabilityProvider& row, std::string* error = nullptr);

    /// 兼容入口: 登记一条 impl 阶段的实现 (未声明时自动声明为 `impls: one` + `impl`)
    bool registerCapability(
        std::string_view                name,
        std::string_view                provider,
        PluginxxCapabilityStartFunction start  = nullptr,
        PluginxxOperatorCancelFunction  cancel = nullptr,
        void*                           ctx    = nullptr
    );

    /// 注销一条实现行; 行不存在返回 false
    bool unregisterProvider(
        std::string_view   id,
        std::string_view   provider,
        std::string_view   ownerTag = {},
        CapabilityPhase_e  phase    = CapabilityPhase_e::Impl
    );

    /// 兼容入口: 注销该插件在这个 id 上的全部行 (含 before / after)
    /// - 槽位没有行之后: 插件声明的删掉, 应用声明的保留 (声明是权威来源)
    bool unregisterCapability(std::string_view name, std::string_view provider);

    /// 移除某个插件的全部实现行 (实例卸载/禁用); 返回移除的行数
    size_t removeProvider(std::string_view provider);

    /// 是否有可用的 impl 行 (只声明、没人实现的能力返回 false)
    bool has(std::string_view id) const;

    /// 首个 impl 行 (兼容旧调用方); 没有返回 nullptr
    const Entry* get(std::string_view id) const;

    /// 首个 impl 行的提供者 (没有返回空串)
    std::string providerOf(std::string_view id) const;

    /// 全部已声明的 id (字典序)
    std::vector<std::string> names() const;

    /// 槽位 (没有声明返回 nullptr)
    const CapabilitySlot* slot(std::string_view id) const;

    /// 某阶段的行, 按 (priority, 登记顺序) 升序; `after` 阶段由执行方倒序
    std::vector<const CapabilityProvider*> rowsOf(
        std::string_view  id,
        CapabilityPhase_e phase
    ) const;

    /// 自省: 一条能力的声明与实现行 (JSON)
    std::string infoJson(std::string_view id) const;

    /// 自省: 全部能力与实现行数 (JSON)
    std::string listJson() const;

    /// 清空 (单元测试用)
    void reset();

    /// 阶段名 (`before` / `impl` / `after` / `unknown`)
    static std::string_view phaseName(CapabilityPhase_e phase) noexcept;

    /// 声明校验; 成功返回空串
    static std::string validateDecl(const CapabilityDecl& decl);

    /// 实现行校验; 成功返回空串
    static std::string validateRow(const CapabilitySlot& slot, const CapabilityProvider& row);

private:

    /// 按 (阶段顺序, priority, seq) 排序; after 与 before 都保持"登记链顺序",
    /// 执行方决定 before 正序 / after 逆序
    static void sortRows(std::vector<CapabilityProvider>& rows);

    /// 重算 `CapabilitySlot::first` (行变化后调用)
    static void refreshFirst(CapabilitySlot& slot);

    std::map<std::string, CapabilitySlot, std::less<>> caps_;
};

} // namespace pluginxx
