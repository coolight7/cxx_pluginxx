#include "pluginxx/host/capability_registry.h"

#include <utility>

namespace pluginxx {
namespace {

/// 阶段在行排序里的相对位置: before → impl → after
int phaseOrder(CapabilityPhase_e phase) noexcept {
    switch (phase) {
        case CapabilityPhase_e::Before:
            return 0;
        case CapabilityPhase_e::Impl:
            return 1;
        case CapabilityPhase_e::After:
            return 2;
    }
    return 3;
}

bool sameRow(const CapabilityProvider& row, std::string_view provider, std::string_view ownerTag,
             CapabilityPhase_e phase) {
    return row.provider == provider && row.ownerTag == ownerTag && row.phase == phase;
}

/// 把行里的阶段写进 JSON (自省用)
utilxx_base::Json rowJson(const CapabilityProvider& row) {
    auto j          = utilxx_base::Json::object();
    j["provider"]   = row.provider;
    if (!row.ownerTag.empty()) {
        j["ownerTag"] = row.ownerTag;
    }
    j["phase"]    = std::string{CapabilityRegistry::phaseName(row.phase)};
    j["priority"] = row.priority;
    j["sync"]     = row.sync;
    return j;
}

} // namespace

std::string_view CapabilityRegistry::phaseName(CapabilityPhase_e phase) noexcept {
    switch (phase) {
        case CapabilityPhase_e::Before:
            return "before";
        case CapabilityPhase_e::Impl:
            return "impl";
        case CapabilityPhase_e::After:
            return "after";
    }
    return "unknown";
}

std::string CapabilityRegistry::validateDecl(const CapabilityDecl& decl) {
    if (decl.id.empty()) {
        return "capability id is empty";
    }
    if (decl.impls != CapabilityImpls_e::One && decl.impls != CapabilityImpls_e::Many) {
        return "capability `" + decl.id + "`: unknown impls value";
    }
    const uint32_t knownPhases
        = kCapabilityPhaseMaskImpl | kCapabilityPhaseMaskBefore | kCapabilityPhaseMaskAfter;
    if (decl.phases == 0 || (decl.phases & ~knownPhases) != 0) {
        return "capability `" + decl.id + "`: phases must be a non-empty subset of before/impl/after";
    }
    if (!decl.openImpl() && decl.impls != CapabilityImpls_e::One) {
        return "capability `" + decl.id + "`: phases without impl require impls: one";
    }
    if (decl.merge != CapabilityMerge_e::None && decl.merge != CapabilityMerge_e::First
        && decl.merge != CapabilityMerge_e::Veto) {
        return "capability `" + decl.id + "`: unknown merge value";
    }
    if (!decl.openImpl() && decl.merge != CapabilityMerge_e::None) {
        return "capability `" + decl.id + "`: merge is meaningless without impl";
    }
    if (decl.openImpl() && decl.impls == CapabilityImpls_e::Many
        && decl.merge == CapabilityMerge_e::None) {
        return "capability `" + decl.id
               + "`: impls: many with impl requires merge: first | veto";
    }
    if (decl.openImpl() && decl.impls == CapabilityImpls_e::One
        && decl.merge == CapabilityMerge_e::Veto) {
        // impls: one 的 impl 行只有一个, 合并没有意义; 写 veto 通常是抄错了声明
        return "capability `" + decl.id + "`: impls: one with impl cannot use merge: veto";
    }
    if (decl.timeoutMs < 0) {
        return "capability `" + decl.id + "`: negative timeoutMs";
    }
    return {};
}

std::string CapabilityRegistry::validateRow(
    const CapabilitySlot&     slot,
    const CapabilityProvider& row
) {
    if (row.provider.empty()) {
        return "capability `" + slot.decl.id + "`: provider name is empty";
    }
    if (!row.start) {
        return "capability `" + slot.decl.id + "`: start callback is null";
    }
    if (!slot.decl.openPhase(row.phase)) {
        return "capability `" + slot.decl.id + "`: phase `"
               + std::string{phaseName(row.phase)} + "` is not open";
    }
    return {};
}

bool CapabilityRegistry::declare(const CapabilityDecl& inDecl, std::string* error) {
    const std::string invalid = validateDecl(inDecl);
    if (!invalid.empty()) {
        if (error) {
            *error = invalid;
        }
        return false;
    }
    auto it = caps_.find(inDecl.id);
    if (it == caps_.end()) {
        CapabilitySlot slot;
        slot.decl = inDecl;
        utilxx_base::insertHeterogeneous(caps_, inDecl.id, std::move(slot));
        return true;
    }

    CapabilitySlot& slot = it->second;
    if (slot.decl.isAppDeclaration() && !inDecl.isAppDeclaration()) {
        if (error) {
            *error = "capability `" + inDecl.id + "` is declared by the application";
        }
        XX_LOGW(
            "CapabilityRegistry: capability `{}` is declared by the application, "
            "plugin declaration ignored",
            inDecl.id
        );
        return false;
    }
    const std::string previous = slot.decl.declaredBy.empty() ? "app" : slot.decl.declaredBy;
    const std::string incoming = inDecl.declaredBy.empty() ? "app" : inDecl.declaredBy;
    XX_LOGI(
        "CapabilityRegistry: declaration of `{}` replaced ({} -> {})",
        inDecl.id,
        previous,
        incoming
    );
    slot.decl = inDecl;
    // 声明收窄后, 已经不符合新声明的行要丢掉 (例如不再开放 after)
    const auto before = slot.rows.size();
    slot.rows.erase(
        std::remove_if(
            slot.rows.begin(),
            slot.rows.end(),
            [&slot](const CapabilityProvider& row) {
                return !slot.decl.openPhase(row.phase);
            }
        ),
        slot.rows.end()
    );
    if (before != slot.rows.size()) {
        XX_LOGW(
            "CapabilityRegistry: {} row(s) of `{}` dropped after declaration change",
            before - slot.rows.size(),
            inDecl.id
        );
    }
    sortRows(slot.rows);
    refreshFirst(slot);
    return true;
}

bool CapabilityRegistry::addProvider(
    std::string_view          id,
    const CapabilityProvider& row,
    std::string*              error
) {
    auto it = caps_.find(id);
    if (it == caps_.end()) {
        if (error) {
            *error = "capability `" + std::string{id} + "` is not declared";
        }
        return false;
    }
    CapabilitySlot& slot      = it->second;
    const std::string invalid = validateRow(slot, row);
    if (!invalid.empty()) {
        if (error) {
            *error = invalid;
        }
        return false;
    }

    CapabilityProvider stored = row;
    if (stored.seq == 0) {
        stored.seq = slot.nextSeq++;
    }

    // impls: one 的 impl 行只有一个: 后登记的 (含跨插件) 整体覆盖
    if (slot.decl.impls == CapabilityImpls_e::One
        && stored.phase == CapabilityPhase_e::Impl) {
        for (const auto& existing : slot.rows) {
            if (existing.phase == CapabilityPhase_e::Impl) {
                XX_LOGI(
                    "CapabilityRegistry: capability `{}` impl replaced ({} -> {})",
                    id,
                    existing.provider,
                    stored.provider
                );
                break;
            }
        }
        slot.rows.erase(
            std::remove_if(
                slot.rows.begin(),
                slot.rows.end(),
                [](const CapabilityProvider& existing) {
                    return existing.phase == CapabilityPhase_e::Impl;
                }
            ),
            slot.rows.end()
        );
    } else {
        // 同 provider + ownerTag + 阶段 = 同一行的重新登记 (等于替换)
        slot.rows.erase(
            std::remove_if(
                slot.rows.begin(),
                slot.rows.end(),
                [&stored](const CapabilityProvider& existing) {
                    return sameRow(
                        existing,
                        stored.provider,
                        stored.ownerTag,
                        stored.phase
                    );
                }
            ),
            slot.rows.end()
        );
    }

    slot.rows.push_back(std::move(stored));
    sortRows(slot.rows);
    refreshFirst(slot);
    return true;
}

bool CapabilityRegistry::registerCapability(
    std::string_view                name,
    std::string_view                provider,
    PluginxxCapabilityStartFunction start,
    PluginxxOperatorCancelFunction  cancel,
    void*                           ctx
) {
    if (name.empty()) {
        return false;
    }
    CapabilityDecl decl;
    decl.id = std::string{name};
    // 兼容入口: 未声明时按"应用声明 + 单实现"处理, 已有声明则沿用
    if (!declares(name)) {
        if (!declare(decl, nullptr)) {
            return false;
        }
    }
    CapabilityProvider row;
    row.provider = std::string{provider};
    row.phase    = CapabilityPhase_e::Impl;
    row.priority = 0;
    row.sync     = false;
    row.start    = start;
    row.cancel   = cancel;
    row.ctx      = ctx;
    if (!addProvider(name, row, nullptr)) {
        return false;
    }
    XX_LOGI(
        "CapabilityRegistry: `{}` registered by plugin `{}`",
        name,
        provider
    );
    return true;
}

bool CapabilityRegistry::unregisterProvider(
    std::string_view  id,
    std::string_view  provider,
    std::string_view  ownerTag,
    CapabilityPhase_e phase
) {
    auto it = caps_.find(id);
    if (it == caps_.end()) {
        return false;
    }
    auto& rows = it->second.rows;
    const auto before = rows.size();
    rows.erase(
        std::remove_if(
            rows.begin(),
            rows.end(),
            [&](const CapabilityProvider& row) {
                return sameRow(row, provider, ownerTag, phase);
            }
        ),
        rows.end()
    );
    if (before == rows.size()) {
        return false;
    }
    refreshFirst(it->second);
    return true;
}

bool CapabilityRegistry::unregisterCapability(std::string_view name, std::string_view provider) {
    auto it = caps_.find(name);
    if (it == caps_.end()) {
        return false;
    }
    auto&      slot   = it->second;
    const auto before = slot.rows.size();
    slot.rows.erase(
        std::remove_if(
            slot.rows.begin(),
            slot.rows.end(),
            [&](const CapabilityProvider& row) {
                return row.provider == provider;
            }
        ),
        slot.rows.end()
    );
    if (before == slot.rows.size()) {
        return false;
    }
    // 插件声明的槽位没有实现行之后就没有意义了; 应用声明的槽位保留 (声明是权威来源)
    if (slot.rows.empty() && !slot.decl.isAppDeclaration()) {
        caps_.erase(it);
        return true;
    }
    refreshFirst(slot);
    return true;
}

size_t CapabilityRegistry::removeProvider(std::string_view provider) {
    size_t removed = 0;
    for (auto it = caps_.begin(); it != caps_.end();) {
        auto&      slot = it->second;
        const auto before = slot.rows.size();
        slot.rows.erase(
            std::remove_if(
                slot.rows.begin(),
                slot.rows.end(),
                [&](const CapabilityProvider& row) {
                    return row.provider == provider;
                }
            ),
            slot.rows.end()
        );
        removed += before - slot.rows.size();
        if (slot.rows.empty() && !slot.decl.isAppDeclaration()) {
            it = caps_.erase(it);
            continue;
        }
        if (before != slot.rows.size()) {
            refreshFirst(slot);
        }
        ++it;
    }
    return removed;
}

bool CapabilityRegistry::declares(std::string_view id) const {
    return caps_.contains(id);
}

bool CapabilityRegistry::has(std::string_view id) const {
    auto it = caps_.find(id);
    if (it == caps_.end()) {
        return false;
    }
    for (const auto& row : it->second.rows) {
        if (row.phase == CapabilityPhase_e::Impl) {
            return true;
        }
    }
    return false;
}

const CapabilityRegistry::Entry* CapabilityRegistry::get(std::string_view id) const {
    auto it = caps_.find(id);
    if (it == caps_.end() || it->second.first.start == nullptr) {
        return nullptr;
    }
    return &it->second.first;
}

std::string CapabilityRegistry::providerOf(std::string_view id) const {
    const auto* entry = get(id);
    return entry ? entry->provider : std::string{};
}

std::vector<std::string> CapabilityRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(caps_.size());
    for (const auto& [id, slot] : caps_) {
        (void)slot;
        out.push_back(id);
    }
    return out;
}

const CapabilitySlot* CapabilityRegistry::slot(std::string_view id) const {
    auto it = caps_.find(id);
    return it == caps_.end() ? nullptr : &it->second;
}

std::vector<const CapabilityProvider*> CapabilityRegistry::rowsOf(
    std::string_view  id,
    CapabilityPhase_e phase
) const {
    std::vector<const CapabilityProvider*> out;
    auto                                   it = caps_.find(id);
    if (it == caps_.end()) {
        return out;
    }
    for (const auto& row : it->second.rows) {
        if (row.phase == phase) {
            out.push_back(&row);
        }
    }
    return out;
}

std::string CapabilityRegistry::infoJson(std::string_view id) const {
    auto it = caps_.find(id);
    if (it == caps_.end()) {
        return {};
    }
    const auto& slot = it->second;
    auto        j    = utilxx_base::Json::object();
    j["id"]          = slot.decl.id;
    j["impls"]       = slot.decl.impls == CapabilityImpls_e::Many ? "many" : "one";
    j["merge"]       = slot.decl.merge == CapabilityMerge_e::Veto ? "veto" : "first";
    auto phases      = utilxx_base::Json::array();
    if (slot.decl.openPhase(CapabilityPhase_e::Before)) {
        phases.push_back("before");
    }
    if (slot.decl.openImpl()) {
        phases.push_back("impl");
    }
    if (slot.decl.openPhase(CapabilityPhase_e::After)) {
        phases.push_back("after");
    }
    j["phases"]    = phases;
    j["timeoutMs"] = slot.decl.timeoutMs;
    j["declaredBy"] = slot.decl.isAppDeclaration() ? std::string{"app"} : slot.decl.declaredBy;
    auto rows       = utilxx_base::Json::array();
    for (const auto& row : slot.rows) {
        rows.push_back(rowJson(row));
    }
    j["providers"] = rows;
    return j.dump();
}

std::string CapabilityRegistry::listJson() const {
    auto out  = utilxx_base::Json::object();
    auto items = utilxx_base::Json::array();
    for (const auto& [id, slot] : caps_) {
        auto item         = utilxx_base::Json::object();
        item["id"]        = id;
        item["impls"]     = slot.decl.impls == CapabilityImpls_e::Many ? "many" : "one";
        item["providers"] = static_cast<int64_t>(slot.rows.size());
        item["implemented"] = slot.first.start != nullptr;
        items.push_back(item);
    }
    out["capabilities"] = items;
    return out.dump();
}

void CapabilityRegistry::reset() {
    caps_.clear();
}

void CapabilityRegistry::sortRows(std::vector<CapabilityProvider>& rows) {
    std::stable_sort(
        rows.begin(),
        rows.end(),
        [](const CapabilityProvider& a, const CapabilityProvider& b) {
            const int pa = phaseOrder(a.phase);
            const int pb = phaseOrder(b.phase);
            if (pa != pb) {
                return pa < pb;
            }
            if (a.priority != b.priority) {
                return a.priority < b.priority;
            }
            return a.seq < b.seq;
        }
    );
}

void CapabilityRegistry::refreshFirst(CapabilitySlot& slot) {
    slot.first = {};
    for (const auto& row : slot.rows) {
        if (row.phase == CapabilityPhase_e::Impl) {
            slot.first.provider = row.provider;
            slot.first.start    = row.start;
            slot.first.cancel   = row.cancel;
            slot.first.ctx      = row.ctx;
            return;
        }
    }
}

} // namespace pluginxx
