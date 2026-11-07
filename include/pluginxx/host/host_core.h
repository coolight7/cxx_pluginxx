/// pluginxx 框架核心 (通用表的状态与方法实现, 与主程序业务无关)
///
/// 定位: 把**通用表** (log / json / config / plugins / events / capabilities /
/// scheduler / coroutine_runtime / tasks / cancel) 的实现从各主程序的管理器里统一到
/// 一处, 主程序只需:
/// 1. 管理器继承本类 (`class MyManager : public pluginxx::PluginHostCore<MyInstance>`);
/// 2. 实现 [DomainHooks] 并调用 [PluginHostCore::setDomainHooks];
/// 3. 用 `pluginxx/host/tables_impl.h` 的 `queryGenericPluginIface` 加载 vtable 的
///    `query_interface` (通用表由插件框架提供, 业务表由主程序自己提供)。
///
/// 归属判据: 通用 = 与"会话/模型/工具/提示词/图"无关的表; 需要主程序语义的数据一律
/// 经 [DomainHooks] 取, 因此本头不包含任何主程序类型。
///
/// 线程约定: 除明确标注的方法外, 全部方法**只在主程序 IO 线程调用** (ABI 入口由
/// `tables_impl.h` 的 trampoline 先投递到 IO 线程); 实例的登记表 (订阅/能力/睡眠
/// 句柄) 因此无需额外加锁。
#pragma once

#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/capability_registry.h"
#include "pluginxx/host/domain_hooks.h"
#include "pluginxx/host/broadcast_registry.h"
#include "pluginxx/host/capability_chain.h"
#include "pluginxx/runtime/manager_base.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pluginxx {
namespace detail {

/// C ABI 字符串视图是否为空 (指针与 (指针,长度) 两种形态)
inline bool abiViewEmpty(const PluginxxStringView* sv) noexcept {
    return !sv || !sv->data || sv->size == 0;
}

inline bool abiViewEmpty(const PluginxxStringView& sv) noexcept {
    return !sv.data || sv.size == 0;
}

/// std::string_view → C ABI 视图 (**借用**; 调用方保证被引用内存长命)
inline PluginxxStringView abiView(std::string_view sv) noexcept {
    return PluginxxStringView{sv.data(), static_cast<uint64_t>(sv.size())};
}

/// 写 C ABI 出参错误串 (主程序堆内存; 插件经 host->free 释放)
inline void setErrorString(PluginxxString* out, std::string_view msg) noexcept {
    if (!out) {
        return;
    }
    hostMemorySetString(out, msg);
}

/// 把 (key, 值) 追加到字符串映射 (能力/订阅等登记表的去重写入)
template<typename Entry, typename Key>
inline void upsertEntry(std::vector<Entry>& entries, const Key& key, Entry&& entry) {
    entries.erase(
        std::remove_if(
            entries.begin(),
            entries.end(),
            [&key](const Entry& existing) {
                return existing.name == key;
            }
        ),
        entries.end()
    );
    entries.push_back(std::forward<Entry>(entry));
}

/// C ABI 视图 → std::string (空视图返回空串)
inline std::string abiViewToString(const PluginxxStringView& sv) {
    if (!sv.data || sv.size == 0) {
        return {};
    }
    return std::string{sv.data, static_cast<size_t>(sv.size)};
}

/// `musicxx.*` = 应用声明的标识
inline bool isOfficialId(std::string_view id) {
    return id.rfind("musicxx.", 0) == 0;
}

/// `plugin.` 开头 = 插件命名空间 (具体归属还要用 [isIdOfPlugin] 判)
inline bool isPluginScopedId(std::string_view id) {
    return id.rfind("plugin.", 0) == 0;
}

/// 带尾点的归属判定: `plugin.<id>.` 才算自己的
/// (带尾点才不会把 `plugin.ab.x` 认成插件 `a` 的)
inline bool isIdOfPlugin(std::string_view id, std::string_view pluginId) {
    if (!isPluginScopedId(id) || pluginId.empty()) {
        return false;
    }
    const std::string prefix = "plugin." + std::string{pluginId} + ".";
    return id.size() > prefix.size() && id.substr(0, prefix.size()) == prefix;
}

/// 插件短名 → 全名 (`drive.list` → `plugin.<id>.drive.list`)
inline std::string qualifyOwnedId(std::string_view pluginId, std::string_view name) {
    std::string out{"plugin."};
    out.append(pluginId);
    out.push_back('.');
    out.append(name);
    return out;
}

inline bool jsonBool(const utilxx_base::Json& j, std::string_view key, bool fallback) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_bool()) {
        return fallback;
    }
    return j[key].get<bool>();
}

inline std::string jsonString(const utilxx_base::Json& j, std::string_view key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) {
        return {};
    }
    return j[key].get<std::string>();
}

inline BroadcastCache_e broadcastCacheOf(const utilxx_base::Json& j) {
    return jsonString(j, "cache") == "latest" ? BroadcastCache_e::Latest
                                              : BroadcastCache_e::None;
}

inline BroadcastWritable_e broadcastWritableOf(const utilxx_base::Json& j) {
    return jsonString(j, "writable") == "anyone" ? BroadcastWritable_e::Anyone
                                                 : BroadcastWritable_e::Owner;
}

inline BroadcastScope_e broadcastScopeOf(const utilxx_base::Json& j) {
    return jsonString(j, "scope") == "system" ? BroadcastScope_e::System
                                              : BroadcastScope_e::App;
}

} // namespace detail

/// 框架核心: 通用表的状态与实现
///
/// - `InstanceT` 须继承 [pluginxx::PluginInstanceBase] (通用表的实例登记表在基类上);
/// - 派生主程序负责业务部分: 业务表的 vtable 入口、加载/卸载/启停事务、业务注册表。
template<typename InstanceT>
class PluginHostCore : public PluginManagerBase<InstanceT> {
public:

    using Base        = PluginManagerBase<InstanceT>;
    using InstancePtr = std::shared_ptr<InstanceT>;

    explicit PluginHostCore(asio::any_io_executor ex = {}) :
        Base(std::move(ex)) {}

    /// 注入业务数据入口
    /// - 主程序管理器实现 [DomainHooks] 后在构造函数体内调用
    ///   (`this->setDomainHooks(this)`), 必须在任何通用表入口被调用之前完成
    void setDomainHooks(DomainHooks* hooks) noexcept {
        hooks_ = hooks;
    }

    DomainHooks* domainHooks() const noexcept {
        return hooks_;
    }

    /// 能力注册表 (主程序其它代码可直接查询)
    std::shared_ptr<CapabilityRegistry> capabilities() const {
        return capabilities_;
    }

    // =====================================================================
    // capabilities 表 (声明 / 登记 / 撤销 / 调用 / 自省)
    // =====================================================================

    /// 广播注册表 (主程序把状态镜像 / 业务事件写进它)
    std::shared_ptr<BroadcastRegistry> broadcasts() const {
        return broadcasts_;
    }

    /// 换成主程序自己的广播注册表 (应用侧预声明频道时用; 不换就用默认的一份)
    void setBroadcastRegistry(std::shared_ptr<BroadcastRegistry> registry) {
        if (registry) {
            broadcasts_ = std::move(registry);
        }
    }

    /// 为应用声明一条能力 (**IO 线程 / 启动阶段**)
    bool declareCapability(const CapabilityDecl& decl, std::string* error = nullptr) {
        return capabilities_->declare(decl, error);
    }

    /// 登记 / 声明一条能力 (**IO 线程**)
    /// - `spec.id` 以 `musicxx.` 开头: 给应用声明过的能力提供实现, 阶段必须在声明里开放;
    /// - 其它写法: 插件自己的名字, 宿主补成 `plugin.<本插件 id>.<名字>`, 未声明时按
    ///   `spec.impls` / `spec.merge` / `spec.phases` 声明;
    /// - `impls: one` 的 impl 行后登记 (含跨插件) 整体覆盖, `impls: many` 多行共存。
    int registerCapability(
        InstanceT*                    inst,
        const PluginxxScopeRef*       scope,
        const PluginxxCapabilitySpec& spec,
        std::string*                  error
    ) {
        (void)scope;
        auto fail = [error](int32_t code, std::string message) {
            if (error) {
                *error = std::move(message);
            }
            return code;
        };
        if (!inst) {
            return fail(PLUGINXX_RESULT_ERR_ARG, "register_capability: missing instance");
        }
        const std::string rawId = detail::abiViewToString(spec.id);
        if (rawId.empty()) {
            return fail(PLUGINXX_RESULT_ERR_ARG, "register_capability: capability id is empty");
        }
        if (!this->acceptsRegistration(inst)) {
            XX_LOGW(
                "Plugin `{}` register_capability rejected: instance is closing or disabled",
                inst->name
            );
            return fail(
                PLUGINXX_RESULT_ERR_ARG,
                "register_capability: instance is closing or disabled"
            );
        }

        const std::string pluginId = this->pluginIdOf(inst);
        const bool        appOwned = detail::isOfficialId(rawId);
        if (!appOwned && detail::isPluginScopedId(rawId)
            && !detail::isIdOfPlugin(rawId, pluginId)) {
            // 写全名时只能占自己的命名空间 (短名由宿主补全, 不会走到这里)
            return fail(
                PLUGINXX_RESULT_ERR_PERMISSION,
                "register_capability: `" + rawId + "` is not in `plugin." + pluginId + ".`"
            );
        }
        const std::string fullId
            = appOwned ? rawId
                       : (detail::isPluginScopedId(rawId)
                              ? rawId
                              : detail::qualifyOwnedId(pluginId, rawId));

        std::string declError;
        if (!capabilities_->declares(fullId) && appOwned) {
            // 官方 id 的声明由应用负责。宿主自己有一份权威声明表时（本项目是生成物
            // `capability_ids.g.h`）先补一条应用声明，免得再开一条"应用下发声明"的通道。
            CapabilityDecl appDecl;
            if (appCapabilityDecl(fullId, &appDecl)) {
                appDecl.id = fullId;
                appDecl.declaredBy.clear(); ///< 应用声明（不是插件声明）
                if (!capabilities_->declare(appDecl, &declError)) {
                    return fail(PLUGINXX_RESULT_ERR_ARG, declError);
                }
            }
        }
        if (!capabilities_->declares(fullId)) {
            if (appOwned) {
                return fail(
                    PLUGINXX_RESULT_ERR_NOT_DECLARED,
                    "register_capability: `" + fullId
                        + "` is not declared by the application"
                );
            }
            CapabilityDecl decl;
            decl.id         = fullId;
            decl.impls      = static_cast<CapabilityImpls_e>(spec.impls);
            decl.merge      = static_cast<CapabilityMerge_e>(spec.merge);
            decl.phases     = spec.phases != 0 ? spec.phases : kCapabilityPhaseMaskImpl;
            decl.timeoutMs  = spec.timeout_ms;
            decl.declaredBy = pluginId;
            if (!capabilities_->declare(decl, &declError)) {
                return fail(PLUGINXX_RESULT_ERR_ARG, declError);
            }
        }

        CapabilityProvider row;
        // provider 用**实例名** (路由键: 一个插件会被各个作用域各挂载一次, 实例名唯一);
        // 归属 / 命名空间校验用插件 id (见上面的 pluginId)
        row.provider = inst->name;
        row.ownerTag = detail::abiViewToString(spec.owner_tag);
        row.phase    = static_cast<CapabilityPhase_e>(
            spec.phase != 0 ? spec.phase : PLUGINXX_CAPABILITY_PHASE_IMPL
        );
        row.priority = spec.priority;
        row.sync     = spec.sync != 0;
        row.start    = spec.run_start;
        row.cancel   = spec.cancel;
        row.ctx      = spec.user_data;
        std::string rowError;
        if (!capabilities_->addProvider(fullId, row, &rowError)) {
            return fail(PLUGINXX_RESULT_ERR_ARG, rowError);
        }
        // 派生宿主把这一行接进自己的派发表（例如 musicxx 的钩子引擎）。
        // 它拒绝时要把刚加的行撤掉，不能留下"注册表有、派发表没有"的半条登记。
        const int32_t attachRc = onCapabilityRowAdded(inst, fullId, spec, &rowError);
        if (attachRc != PLUGINXX_RESULT_OK) {
            capabilities_->unregisterProvider(fullId, inst->name, row.ownerTag);
            return fail(attachRc, rowError.empty() ? "register_capability: rejected by host" : rowError);
        }
        detail::upsertEntry(
            inst->capabilityRegistrations,
            fullId,
            PluginCapabilityRegistration{
                fullId,
                spec.run_start,
                spec.cancel,
                spec.user_data
            }
        );
        return PLUGINXX_RESULT_OK;
    }

    /// 注销本插件的一条实现行 (**IO 线程**; `ownerTag` 空 = 所有阶段里没写 owner_tag 的行)
    int unregisterCapability(
        InstanceT*       inst,
        std::string_view capability,
        std::string_view ownerTag
    ) {
        if (!inst || capability.empty()) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        const std::string pluginId = this->pluginIdOf(inst);
        const std::string rawId{capability};
        const std::string fullId
            = detail::isOfficialId(rawId)
                  ? rawId
                  : (detail::isPluginScopedId(rawId)
                         ? rawId
                         : detail::qualifyOwnedId(pluginId, rawId));
        if (!capabilities_->unregisterProvider(fullId, inst->name, ownerTag)) {
            return PLUGINXX_RESULT_ERR_NOT_DECLARED;
        }
        onCapabilityRowRemoved(inst, fullId, ownerTag);
        return PLUGINXX_RESULT_OK;
    }

    /// 能力是否可用 (1 = 有可用的 impl 行)
    int hasCapability(std::string_view capability) const {
        if (capability.empty()) {
            return 0;
        }
        return capabilities_->has(capability) ? 1 : 0;
    }

    /// 撤销实例声明的全部能力 (**IO 线程**; 保留实例内的登记记录, 供启用事务重新声明)
    void unregisterInstanceCapabilities(InstanceT* inst) {
        if (!inst) {
            return;
        }
        capabilities_->removeProvider(inst->name);
    }

    /// 撤销实例声明的全部能力并清空登记记录 (**IO 线程**; 实例即将销毁/停用收尾)
    void clearInstanceCapabilities(InstanceT* inst) {
        if (!inst) {
            return;
        }
        unregisterInstanceCapabilities(inst);
        inst->capabilityRegistrations.clear();
    }

    /// 调用能力 (**IO 线程**; ABI 入口已把跨线程调用投递过来)
    /// - `phase` 为 0: 走完整链 before → impl → after; 否则只跑该阶段;
    /// - `after` 阶段倒序执行; 每一行各自持有提供者的执行 lease;
    /// - `timeoutMs` 是本次调用的超时 (<0 = 用声明里的值): 只对应用实现的能力生效
    ///   (插件行各有自己的超时语义);
    /// - 完成通知经 [OpCore] 恒在 IO 线程发布 (即使插件同步 done);
    /// - 真正拒绝 (没有声明 / 没有可用实现) 返回 NULL 并写 `error_out`, 此时不调用 `cb`。
    PluginxxOperatorHandle* invokeCapabilityAsync(
        InstanceT*               caller,
        const PluginxxScopeRef*  scope,
        std::string_view         capability,
        std::string_view         argsJson,
        int32_t                  phase,
        int32_t                  timeoutMs,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    ) {
        (void)scope;
        std::shared_ptr<CapabilityChain> chain;
        std::shared_ptr<OpCore>          core;
        try {
            if (!caller || !this->ioExecutor()) {
                throw std::runtime_error(
                    "invoke_capability: missing caller or IO executor"
                );
            }
            const std::string cap{capability};
            const auto*       slot = capabilities_->slot(cap);
            if (!slot) {
                throw std::runtime_error(
                    "invoke_capability: capability is not declared: " + cap
                );
            }
            std::vector<CapabilityProvider> rows;
            if (phase == 0) {
                detail::appendStage(
                    rows,
                    *capabilities_,
                    cap,
                    CapabilityPhase_e::Before,
                    false
                );
                detail::appendStage(
                    rows,
                    *capabilities_,
                    cap,
                    CapabilityPhase_e::Impl,
                    false
                );
                detail::appendStage(
                    rows,
                    *capabilities_,
                    cap,
                    CapabilityPhase_e::After,
                    true
                );
            } else {
                const auto wanted = static_cast<CapabilityPhase_e>(phase);
                detail::appendStage(
                    rows,
                    *capabilities_,
                    cap,
                    wanted,
                    wanted == CapabilityPhase_e::After
                );
            }
            if (rows.empty() && !slot->decl.declaredBy.empty()) {
                throw std::runtime_error(
                    "invoke_capability: capability has no implementation: " + cap
                );
            }
            auto owner = caller->template sharedSelf<InstanceT>();
            if (!owner) {
                throw std::runtime_error(
                    "invoke_capability: caller instance is unavailable"
                );
            }
            if (rows.empty()) {
                /// 应用实现的能力（声明由应用给出、没有插件实现行）: 派发前把调用
                /// 交给派生宿主转给应用（musicxx 的动作就是这种）。完成与取消都
                /// 复用同一套 Operation 协议。
                core = OpCore::create(this->runtime(), owner, nullptr, "capability " + cap);
                core->setCallback(cb, ud);
                const std::string argsText{argsJson};
                const int32_t     appTimeoutMs =
                    timeoutMs >= 0 ? timeoutMs : slot->decl.timeoutMs;
                OpDrive           appDrive;
                appDrive.start = [this, caller, cap, argsText, appTimeoutMs](
                                     const PluginxxOperatorNotify* notify,
                                     PluginxxString*               errorOut
                                 ) -> void* {
                    return this->onAppCapabilityStart(
                        caller, cap, argsText, appTimeoutMs, notify, errorOut
                    );
                };
                appDrive.cancel = [this, caller, cap](void* handle) {
                    this->onAppCapabilityCancel(caller, cap, handle);
                };
                std::string startError;
                if (!core->start(std::move(appDrive), startError)) {
                    detail::setErrorString(error_out, startError);
                    return nullptr;
                }
                return core->handle();
            }
            // 外层操作只持有调用方的 lease: 一次调用是调用方的一次操作, 每一行
            // 在启动时各自持有提供者的 lease (提供者卸载会挡住或跳过它)。
            core = OpCore::create(this->runtime(), owner, nullptr, "capability " + cap);
            chain             = std::make_shared<CapabilityChain>();
            chain->host       = this;
            chain->core       = core;
            chain->caller     = owner;
            chain->capability = cap;
            chain->merge      = slot->decl.merge;
            chain->rows       = std::move(rows);
            chain->args
                = argsJson.empty() ? std::string{"{}"} : std::string{argsJson};
            core->setCallback(cb, ud);
            core->accept([weak = std::weak_ptr<CapabilityChain>(chain)] {
                if (auto self = weak.lock()) {
                    self->cancelCurrent();
                }
            });
            chain->startNext();
            return core->handle();
        } catch (const std::exception& e) {
            if (core && !core->submitted()) {
                core->reject();
            }
            detail::setErrorString(error_out, e.what());
            return nullptr;
        }
    }

    /// 自省: 一条能力 (**IO 线程**; 未声明返回 [PLUGINXX_RESULT_ERR_NOT_DECLARED])
    int capabilityInfo(std::string_view capability, PluginxxString* outJson) {
        const std::string text = capabilities_->infoJson(capability);
        if (text.empty()) {
            return PLUGINXX_RESULT_ERR_NOT_DECLARED;
        }
        detail::setErrorString(outJson, text);
        return PLUGINXX_RESULT_OK;
    }

    /// 自省: 全部能力 (**IO 线程**)
    int listCapabilities(PluginxxString* outJson) {
        detail::setErrorString(outJson, capabilities_->listJson());
        return PLUGINXX_RESULT_OK;
    }

    // =====================================================================
    // broadcasts 表 (声明 / 发布 / 订阅 / 读当前值)
    // =====================================================================

    /// 声明频道 (**IO 线程 / 启动阶段**; JSON 形如
    /// `{"id": "...", "cache": "latest", "writable": "owner",
    ///   "pushOnSubscribe": true, "coalesce": true, "scope": "app"}`)
    int declareBroadcast(InstanceT* inst, std::string_view declJson, std::string* error) {
        detail::ParsedJson parsed = detail::parseJson(declJson);
        if (!parsed.ok || !parsed.value.is_object()) {
            if (error) {
                *error = "declare_broadcast: declaration must be a JSON object";
            }
            return PLUGINXX_RESULT_ERR_ARG;
        }
        return declareBroadcastJson(inst, parsed.value, error);
    }

    /// 声明频道 (已解析的 JSON; 主程序自己声明频道时用)
    int declareBroadcastJson(
        InstanceT*                 inst,
        const utilxx_base::Json&   spec,
        std::string*               error
    ) {
        if (!spec.is_object()) {
            if (error) {
                *error = "declare_broadcast: declaration must be a JSON object";
            }
            return PLUGINXX_RESULT_ERR_ARG;
        }
        BroadcastDecl decl;
        decl.id = detail::jsonString(spec, "id");
        if (decl.id.empty()) {
            if (error) {
                *error = "declare_broadcast: id is required";
            }
            return PLUGINXX_RESULT_ERR_ARG;
        }
        if (inst) {
            const std::string pluginId = this->pluginIdOf(inst);
            if (!detail::isOfficialId(decl.id) && !detail::isPluginScopedId(decl.id)) {
                decl.id = detail::qualifyOwnedId(pluginId, decl.id);
            } else if (detail::isPluginScopedId(decl.id)
                       && !detail::isIdOfPlugin(decl.id, pluginId)) {
                if (error) {
                    *error = "declare_broadcast: `" + decl.id + "` is not in `plugin."
                             + pluginId + ".`";
                }
                return PLUGINXX_RESULT_ERR_PERMISSION;
            }
            decl.owner = pluginId;
        }
        decl.cache           = detail::broadcastCacheOf(spec);
        decl.writable        = detail::broadcastWritableOf(spec);
        decl.pushOnSubscribe = detail::jsonBool(spec, "pushOnSubscribe", false);
        decl.coalesce        = detail::jsonBool(spec, "coalesce", true);
        decl.scope           = detail::broadcastScopeOf(spec);
        std::string declError;
        if (!broadcasts_->declare(decl, &declError)) {
            if (error) {
                *error = declError;
            }
            return PLUGINXX_RESULT_ERR_ARG;
        }
        return PLUGINXX_RESULT_OK;
    }

    /// 发布 (**IO 线程**; `writer` 为插件 id, 空 = 应用)
    int publishBroadcast(
        std::string_view broadcastId,
        std::string_view payloadJson,
        std::string_view writer,
        std::string*     error
    ) {
        std::string reason;
        if (!broadcasts_->publish(broadcastId, payloadJson, writer, &reason)) {
            if (error) {
                *error = reason;
            }
            const bool permission = reason.find("system") != std::string::npos
                                    || reason.find("owned by") != std::string::npos;
            return permission ? PLUGINXX_RESULT_ERR_PERMISSION : PLUGINXX_RESULT_ERR_NOT_DECLARED;
        }
        return PLUGINXX_RESULT_OK;
    }

    /// 插件发布 (**IO 线程**; 归属用插件 id)
    int publishBroadcastFrom(
        InstanceT*       inst,
        std::string_view broadcastId,
        std::string_view payloadJson,
        std::string*     error
    ) {
        return publishBroadcast(broadcastId, payloadJson, pluginIdOf(inst), error);
    }

    /// 读当前值 (**任意线程**; 注册表内部加锁)
    int readBroadcast(std::string_view broadcastId, PluginxxString* outJson) const {
        std::string value;
        if (!broadcasts_->read(broadcastId, &value)) {
            return broadcasts_->declares(broadcastId) ? PLUGINXX_RESULT_ERR_NO_VALUE
                                                      : PLUGINXX_RESULT_ERR_NOT_DECLARED;
        }
        detail::setErrorString(outJson, value);
        return PLUGINXX_RESULT_OK;
    }

    /// 订阅广播 (**IO 线程**)
    /// - `id` 经 [DomainHooks::qualifyEventTopic] 补齐命名空间;
    /// - 回调在发布线程执行, 每次回调都在实例执行 lease 保护下调用插件
    ///   (因此卸载的 idle 等待覆盖它);
    /// - 实例正在关闭/已禁用, 或 id 非法时返回 nullptr。
    PluginxxSubscription* subscribeBroadcast(
        InstanceT*               inst,
        const PluginxxScopeRef*  scope,
        std::string_view         broadcastId,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payload_json, void* ud),
        void* ud
    ) {
        if (!inst || broadcastId.empty() || !handler) {
            return nullptr;
        }
        if (!this->acceptsRegistration(inst)) {
            XX_LOGW(
                "Plugin `{}` subscribe_broadcast rejected: instance is closing or disabled",
                inst->name
            );
            return nullptr;
        }
        const std::string fullId
            = hooks_ ? hooks_->qualifyEventTopic(broadcastId)
                     : std::string{broadcastId};
        if (fullId.empty()) {
            XX_LOGW(
                "Plugin `{}` subscribe_broadcast rejected: illegal id `{}`",
                inst->name,
                broadcastId
            );
            return nullptr;
        }

        auto sub         = std::make_shared<PluginxxSubscription>();
        sub->registry    = broadcasts_;
        sub->broadcastId = fullId;
        sub->inst        = inst->ownerSelf;
        sub->runtime     = this->runtime();
        sub->handler     = handler;
        sub->ud          = ud;
        if (scope && scope->id.data && scope->id.size > 0) {
            sub->scopeId.assign(scope->id.data, static_cast<size_t>(scope->id.size));
        }

        const uint64_t token = broadcasts_->subscribe(
            fullId,
            std::string{},
            [sub](std::string_view data) {
                if (!sub->alive.load(std::memory_order_acquire) || !sub->handler) {
                    return;
                }
                auto inst = sub->inst.lock();
                if (!inst || !inst->enabled || !inst->lifetime
                    || !inst->lifetime->acceptsOperations()) {
                    return;
                }
                PluginInstanceBase::InflightGuard guard(inst);
                if (!guard || !sub->alive.load(std::memory_order_acquire)) {
                    return;
                }
                try {
                    const auto view = detail::abiView(data);
                    sub->handler(&view, sub->ud);
                } catch (const std::exception& e) {
                    XX_LOGW("Plugin `{}` broadcast handler threw: {}", inst->name, e.what());
                } catch (...) {
                    XX_LOGW("Plugin `{}` broadcast handler threw unknown exception", inst->name);
                }
            }
        );
        if (token == 0) {
            XX_LOGW(
                "Plugin `{}` subscribe_broadcast failed: broadcast `{}` rejected",
                inst->name,
                fullId
            );
            return nullptr;
        }
        sub->token = token;
        inst->subscriptionHandles.push_back(sub);
        inst->subscriptions.push_back(sub);
        return sub.get();
    }

    /// 撤销广播订阅 (**幂等**; 可在任意线程调用, 簿记自动回到 IO 线程)
    void unsubscribe(PluginxxSubscription* sub) {
        unsubscribePluginBroadcast(sub);
    }

    /// 撤销实例的全部广播订阅 (**IO 线程**; 不做注册表往返, 直接令句柄失效)
    /// - 卸载 / 禁用路径调用; 句柄本体仍由实例保活, 插件持有的裸指针继续安全
    void revokeInstanceSubscriptions(InstanceT* inst) {
        if (!inst) {
            return;
        }
        // 先快照再逐个撤销: [detail::revokeSubscription] 会从实例的订阅表里移除句柄,
        // 不能边遍历同一个 vector 边删除。
        auto subs = inst->subscriptions;
        for (const auto& sub : subs) {
            if (!sub) {
                continue;
            }
            sub->alive.store(false, std::memory_order_release);
            detail::revokeSubscription(sub.get());
        }
        inst->subscriptions.clear();
    }

    // =====================================================================
    // scheduler 表 (IO 投递 / 睡眠 / 阻塞工作委托)
    // =====================================================================

    /// 投递一次回调到 IO 线程 (**IO 线程**)
    /// - 恒异步: 回调经 OpCore 完成协议在 IO 线程发布, 因此不会在调用栈内重入
    PluginxxOperatorHandle*
        postCallback(InstanceT* inst, void(PLUGINXX_CALL* fn)(void*), void* ud) {
        if (!inst || !fn || !this->isIoThread()) {
            return nullptr;
        }
        auto core = OpCore::create(
            this->runtime(),
            inst->template sharedSelf<InstanceT>(),
            nullptr,
            "scheduler post"
        );
        try {
            core->setCompletionHandler([fn, ud](int32_t, std::string_view) {
                fn(ud);
            });
            core->accept();
            auto notify = core->notify();
            notify.done(notify.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            return core->handle();
        } catch (...) {
            core->reject();
            throw;
        }
    }

    /// 定时器睡眠 (**IO 线程**); 完成/取消/失败都经完成回调恰好一次上报
    PluginxxOperatorHandle* sleep(
        InstanceT*               inst,
        int64_t                  ms,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    ) {
        if (!inst || !cb || !this->isIoThread()) {
            detail::setErrorString(
                error_out,
                "scheduler sleep: invalid instance, callback, or thread"
            );
            return nullptr;
        }
        if (ms < 0) {
            detail::setErrorString(error_out, "scheduler sleep: negative duration");
            return nullptr;
        }

        auto owner = inst->template sharedSelf<InstanceT>();
        if (!owner) {
            detail::setErrorString(error_out, "scheduler sleep: instance is unavailable");
            return nullptr;
        }

        std::shared_ptr<OpCore> core;
        try {
            core         = OpCore::create(this->runtime(), owner, nullptr, "scheduler sleep");
            auto  timer  = std::make_shared<asio::steady_timer>(this->ioExecutor());
            auto* handle = core->handle();
            core->setCallback(cb, ud);
            core->setCompletionHandler([owner, handle](int32_t, std::string_view) {
                owner->sleepTimers.erase(handle);
            });
            core->accept([timer] {
                timer->cancel();
            });
            owner->sleepTimers.emplace(handle, handle->shared_from_this());
            timer->expires_after(std::chrono::milliseconds(ms));
            timer->async_wait([core, timer](const utilxx_base::AsioErrorCode& ec) {
                auto notify = core->notify();
                notify.done(
                    notify.host_ud,
                    ec ? PLUGINXX_OPERATOR_CANCELED : PLUGINXX_OPERATOR_OK,
                    nullptr
                );
            });
            return handle;
        } catch (const std::exception& e) {
            if (core) {
                owner->sleepTimers.erase(core->handle());
                core->reject();
            }
            detail::setErrorString(error_out, e.what());
            return nullptr;
        } catch (...) {
            if (core) {
                owner->sleepTimers.erase(core->handle());
                core->reject();
            }
            detail::setErrorString(error_out, "scheduler sleep: failed to create timer");
            return nullptr;
        }
    }

    /// 阻塞工作委托到主程序工作线程池 (**IO 线程**)
    /// - 工作体本身在工作线程执行 (显式例外), 完成通知经 OpCore 回到 IO 线程;
    /// - `work` 收到的取消令牌只在本次工作调用期间有效 (插件不得保存);
    /// - 主程序无工作线程时立即以失败终结 (不静默丢工作)。
    PluginxxOperatorHandle* offload(
        InstanceT* inst,
        void*(PLUGINXX_CALL*
                  work)(void* ud, const PluginxxCancelToken* token, PluginxxString* error_out),
        void(PLUGINXX_CALL*
                 done)(void* ud, int32_t status, void* result, const PluginxxStringView* error),
        void*           ud,
        PluginxxString* error_out
    ) {
        if (!inst || !work || !this->isIoThread()) {
            detail::setErrorString(
                error_out,
                "scheduler offload: invalid instance, work, or thread"
            );
            return nullptr;
        }

        auto owner = inst->template sharedSelf<InstanceT>();
        if (!owner) {
            detail::setErrorString(error_out, "scheduler offload: instance is unavailable");
            return nullptr;
        }

        struct WorkResult {
            void* value = nullptr;
        };

        std::shared_ptr<OpCore> core;
        try {
            core             = OpCore::create(this->runtime(), owner, nullptr, "scheduler offload");
            auto result      = std::make_shared<WorkResult>();
            auto cancelState = std::make_shared<OffloadCancelState>();
            core->setCompletionHandler([result, done, ud](int32_t status, std::string_view error) {
                if (!done) {
                    return;
                }
                auto view = detail::abiView(error);
                if (status == PLUGINXX_OPERATOR_FAILED && error.empty()) {
                    view = detail::abiView("plugin offload failed");
                }
                done(ud, status, result->value, &view);
            });
            core->accept([cancelState] {
                cancelState->requested.store(true, std::memory_order_release);
            });

            const bool queued = [&]() {
                if (!hooks_ || !hooks_->postToWorkerThread([core, result, cancelState, work, ud]() {
                        PluginxxCancelToken token{&isOffloadCanceled, cancelState.get()};
                        PluginxxString      workError{};
                        int32_t             status = PLUGINXX_OPERATOR_OK;
                        std::string         error;
                        try {
                            result->value = work(ud, &token, &workError);
                            if (workError.data) {
                                error.assign(workError.data, static_cast<size_t>(workError.size));
                                status = PLUGINXX_OPERATOR_FAILED;
                            }
                            if (cancelState->requested.load(std::memory_order_acquire)) {
                                status = PLUGINXX_OPERATOR_CANCELED;
                            }
                        } catch (const std::exception& e) {
                            status = PLUGINXX_OPERATOR_FAILED;
                            try {
                                error = e.what();
                            } catch (...) {
                            }
                        } catch (...) {
                            status = PLUGINXX_OPERATOR_FAILED;
                            error  = "plugin offload worker threw unknown exception";
                        }
                        hostMemoryFree(workError.data);
                        auto notify = core->notify();
                        auto view   = detail::abiView(error);
                        notify.done(notify.host_ud, status, &view);
                    })) {
                    return false;
                }
                return true;
            }();

            if (!queued) {
                auto notify = core->notify();
                auto view   = detail::abiView("plugin offload: no thread pool");
                notify.done(notify.host_ud, PLUGINXX_OPERATOR_FAILED, &view);
            }
            return core->handle();
        } catch (const std::exception& e) {
            if (core) {
                core->reject();
            }
            detail::setErrorString(error_out, e.what());
            return nullptr;
        } catch (...) {
            if (core) {
                core->reject();
            }
            detail::setErrorString(error_out, "scheduler offload: failed to create operation");
            return nullptr;
        }
    }

    // =====================================================================
    // tasks 表 (后台任务托管)
    // =====================================================================

    /// 托管一个后台任务 (**IO 线程**)
    /// - `cancel_fn`/`cancel_ud`: 卸载取消时主程序回调 (IO 线程, 协作式);
    /// - `notify`: 【出参】插件协程结束 (帧销毁后) 经 `notify.done` 恰好一次上报;
    /// - 返回主程序托管句柄 (失败 NULL + error_out); 句柄仅用于 `cancel_task`,
    ///   主程序在任务 done 后自动回收。
    PluginxxOperatorHandle* registerTask(
        InstanceT*                     inst,
        PluginxxOperatorCancelFunction cancel_fn,
        void*                          cancel_ud,
        PluginxxOperatorNotify*        notify,
        PluginxxString*                error_out
    ) {
        if (notify) {
            *notify = {};
        }
        std::shared_ptr<OpCore> core;
        try {
            if (!inst || !notify || !this->isIoThread() || !this->acceptsRegistration(inst)) {
                throw std::runtime_error("register_task: missing instance/notify or wrong IO thread"
                );
            }
            core = OpCore::create(
                this->runtime(),
                inst->template sharedSelf<InstanceT>(),
                nullptr,
                "background task"
            );
            core->accept([cancel_fn, cancel_ud] {
                if (cancel_fn) {
                    cancel_fn(cancel_ud, nullptr);
                }
            });
            *notify = core->notify();
            return core->handle();
        } catch (const std::exception& e) {
            if (core) {
                core->reject();
            }
            detail::setErrorString(error_out, e.what());
            return nullptr;
        }
    }

protected:

    /// 一条能力实现行登记成功后的挂接点（派生宿主把业务派发表接进来时用）
    ///
    /// 默认什么都不做（纯 pluginxx 宿主只记注册表）。返回非 0 时调用方会撤销刚加的
    /// 实现行并把原因返回给插件 —— 用于"这个 id 我认识，但这次登记不合法"这类判定
    /// （例如钩子只接受同步处理器）。
    virtual int onCapabilityRowAdded(
        InstanceT*                    inst,
        const std::string&            id,
        const PluginxxCapabilitySpec& spec,
        std::string*                  error
    ) {
        (void)inst;
        (void)id;
        (void)spec;
        (void)error;
        return PLUGINXX_RESULT_OK;
    }

    /// 一条能力实现行撤销后的挂接点（与 [onCapabilityRowAdded] 成对）
    ///
    /// 实例卸载 / 停用走的是一次性清理（`unregisterInstanceCapabilities`），那里不逐行
    /// 回调：派生宿主的业务表按实例整体清（musicxx 是 `clearPluginRegistrations`）。
    virtual void onCapabilityRowRemoved(
        InstanceT*         inst,
        const std::string& id,
        std::string_view   ownerTag
    ) {
        (void)inst;
        (void)id;
        (void)ownerTag;
    }

    /// 官方 id（`musicxx.*`）的声明来源（宿主自己有一份权威表时用）
    ///
    /// `register_capability` 对官方 id 要求"应用已声明"。宿主在 [out] 里填好声明并
    /// 返回 true 时，调用方会把它登记成应用声明；返回 false 表示宿主也不知道，
    /// 按原来的规则拒绝。
    virtual bool appCapabilityDecl(std::string_view id, CapabilityDecl* out) {
        (void)id;
        (void)out;
        return false;
    }

    /// 应用实现的能力被调用时把调用接走（能力由应用声明、没有插件实现行）
    ///
    /// 应用实现的能力（musicxx 的动作就是这种）没有插件提供者，所以派发前先把
    /// 调用交给派生宿主：宿主把它转给应用，应用回答后调 `notify->done` 恰好一次。
    /// 返回的句柄只用于取消（为空表示不可取消）；返回空、没 done 也没写 `error_out`
    /// 时调用方按"没人接"拒绝。只在 IO 线程调用。`timeoutMs` 是声明里的超时（0 = 不设）。
    virtual void* onAppCapabilityStart(
        InstanceT*                    caller,
        const std::string&            capability,
        std::string_view              argsJson,
        int32_t                       timeoutMs,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    ) {
        (void)caller;
        (void)argsJson;
        (void)timeoutMs;
        (void)notify;
        if (error_out) {
            detail::setErrorString(
                error_out,
                "capability `" + capability
                    + "` is declared by the application, but the host did not take the call"
            );
        }
        return nullptr;
    }

    /// 取消一次应用能力调用（`handle` 是 [onAppCapabilityStart] 的返回值）
    virtual void onAppCapabilityCancel(
        InstanceT*         caller,
        const std::string& capability,
        void*              handle
    ) {
        (void)caller;
        (void)capability;
        (void)handle;
    }

    /// offload 取消状态: 令牌只在工作调用期间有效, 插件不得保存 token 或 host_ud
    struct OffloadCancelState {
        std::atomic<bool> requested{false};
    };

    static int32_t PLUGINXX_CALL isOffloadCanceled(const PluginxxCancelToken* token) {
        auto* state = token ? static_cast<const OffloadCancelState*>(token->host_ud) : nullptr;
        return state && state->requested.load(std::memory_order_acquire) ? 1 : 0;
    }

    /// 实例名 → 插件 id (能力 / 广播的归属用插件 id, 不用实例名)
    std::string pluginIdOf(const InstanceT* inst) const {
        if (!inst) {
            return {};
        }
        return hooks_ ? hooks_->pluginId(inst->name) : inst->name;
    }

    /// 一次能力调用的阶段链状态 (内部类型)
    ///
    /// 执行模型:
    /// - 每行启动时各自持有一份提供者的执行 lease, 外层操作持有调用方的 lease;
    /// - `before` 行的回答作为补丁合并进参数; `impl` 行按 `merge` 合并;
    ///   `after` 行倒序执行, 回答合并进结果;
    /// - 特殊键: `cancel` / `stop` 终止并回取消, `disable` 让能力不生效 (结果为 null),
    ///   `drop` (after) 丢弃当前结果, `patch` 是补丁体 (没有它时顶层对象即补丁)。
    struct CapabilityChain : std::enable_shared_from_this<CapabilityChain> {
        PluginHostCore*                 host = nullptr;
        std::shared_ptr<OpCore>         core;
        std::shared_ptr<InstanceT>      caller;
        std::string                     capability;
        CapabilityMerge_e               merge = CapabilityMerge_e::None;
        std::vector<CapabilityProvider> rows;
        size_t                          index = 0;
        std::string                     args;
        std::string                     result;
        bool                            hasResult = false;
        bool                            finished  = false;
        std::shared_ptr<OpCore>         current;

        void cancelCurrent() {
            if (current) {
                current->cancel();
            }
        }

        void finish(int32_t status, std::string payload) {
            if (finished) {
                return;
            }
            finished        = true;
            auto       notify = core->notify();
            const auto view   = detail::abiView(payload);
            notify.done(notify.host_ud, status, &view);
        }

        void startNext() {
            if (finished) {
                return;
            }
            while (index < rows.size()) {
                const CapabilityProvider row = rows[index];
                auto provider = host->find(row.provider);
                if (!provider || !provider->enabled) {
                    XX_LOGW(
                        "capability `{}`: provider `{}` is not available, skipped",
                        capability,
                        row.provider
                    );
                    ++index;
                    continue;
                }
                std::shared_ptr<OpCore> op;
                try {
                    op = OpCore::create(
                        host->runtime(),
                        provider,
                        caller,
                        "capability " + capability
                    );
                } catch (const std::exception& e) {
                    XX_LOGW(
                        "capability `{}`: cannot start provider `{}`: {}",
                        capability,
                        row.provider,
                        e.what()
                    );
                    ++index;
                    continue;
                }
                current = op;
                auto self = this->shared_from_this();
                op->setCompletionHandler(
                    [self, row](int32_t status, std::string_view payload) {
                        self->onRowDone(row, status, payload);
                    }
                );
                std::string input = args;
                if (row.phase == CapabilityPhase_e::After) {
                    auto parsedArgs = detail::parseJson(args);
                    auto parsedRes  = detail::parseJson(result);
                    utilxx_base::Json wrapper = utilxx_base::Json::object();
                    wrapper["args"] = parsedArgs.ok ? parsedArgs.value
                                                    : utilxx_base::Json(nullptr);
                    wrapper["result"]
                        = (hasResult && parsedRes.ok) ? parsedRes.value
                                                      : utilxx_base::Json(nullptr);
                    input = wrapper.dump();
                }
                const std::string capCopy = capability;
                OpDrive           drive;
                drive.start = [row, input, capCopy](
                                  const PluginxxOperatorNotify* notify,
                                  PluginxxString*               error
                              ) -> void* {
                    const auto idView   = detail::abiView(capCopy);
                    const auto argsView = detail::abiView(input);
                    return row.start(row.ctx, nullptr, &idView, &argsView, notify, error);
                };
                drive.cancel = [row](void* opHandle) {
                    if (row.cancel) {
                        row.cancel(row.ctx, opHandle);
                    }
                };
                std::string error;
                if (!op->start(std::move(drive), error)) {
                    current.reset();
                    if (row.phase == CapabilityPhase_e::Impl) {
                        finish(PLUGINXX_OPERATOR_FAILED, error);
                        return;
                    }
                    XX_LOGW(
                        "capability `{}`: provider `{}` did not start: {}",
                        capability,
                        row.provider,
                        error
                    );
                    ++index;
                    continue;
                }
                return;
            }
            finish(PLUGINXX_OPERATOR_OK, hasResult ? result : std::string{"null"});
        }

        void onRowDone(
            const CapabilityProvider& row,
            int32_t                   status,
            std::string_view          payload
        ) {
            current.reset();
            ++index;
            if (finished) {
                return;
            }
            if (status == PLUGINXX_OPERATOR_FAILED) {
                finish(PLUGINXX_OPERATOR_FAILED, std::string{payload});
                return;
            }
            if (status == PLUGINXX_OPERATOR_CANCELED) {
                finish(PLUGINXX_OPERATOR_CANCELED, std::string{payload});
                return;
            }

            const auto parsed   = detail::parseJson(payload);
            const bool hasValue = parsed.ok && !parsed.value.is_null();
            const auto patch    = [&]() -> utilxx_base::Json {
                if (hasValue && parsed.value.is_object()
                    && parsed.value.contains("patch") && parsed.value["patch"].is_object()) {
                    return parsed.value["patch"];
                }
                return hasValue ? parsed.value : utilxx_base::Json(nullptr);
            }();
            if (!hasValue) {
                startNext();
                return;
            }
            const bool cancel = detail::jsonBool(parsed.value, "cancel", false);
            const bool stop   = detail::jsonBool(parsed.value, "stop", false);
            const bool disable = detail::jsonBool(parsed.value, "disable", false);
            const bool drop    = detail::jsonBool(parsed.value, "drop", false);

            switch (row.phase) {
                case CapabilityPhase_e::Before: {
                    if (cancel || stop) {
                        finish(PLUGINXX_OPERATOR_CANCELED, std::string{payload});
                        return;
                    }
                    if (!patch.is_null()) {
                        auto currentArgs = detail::parseJson(args);
                        utilxx_base::Json next = currentArgs.ok
                                                     ? currentArgs.value
                                                     : utilxx_base::Json::object();
                        detail::mergeJsonPatch(next, patch);
                        args = next.dump();
                    }
                    break;
                }
                case CapabilityPhase_e::Impl: {
                    if (cancel || stop) {
                        finish(PLUGINXX_OPERATOR_CANCELED, std::string{payload});
                        return;
                    }
                    if (disable) {
                        // {"disable": true}: 这条能力不生效, 结果为空
                        hasResult = false;
                        result.clear();
                        finish(PLUGINXX_OPERATOR_OK, std::string{"null"});
                        return;
                    }
                    if (merge == CapabilityMerge_e::First) {
                        result    = patch.is_null() ? parsed.value.dump() : patch.dump();
                        hasResult = true;
                        finish(PLUGINXX_OPERATOR_OK, result);
                        return;
                    }
                    if (hasResult) {
                        auto currentResult = detail::parseJson(result);
                        utilxx_base::Json next = currentResult.ok
                                                     ? currentResult.value
                                                     : utilxx_base::Json(nullptr);
                        detail::mergeJsonPatch(next, patch);
                        result = next.dump();
                    } else {
                        result    = patch.is_null() ? std::string{"null"} : patch.dump();
                        hasResult = true;
                    }
                    break;
                }
                case CapabilityPhase_e::After: {
                    if (drop) {
                        hasResult = false;
                        result.clear();
                        break;
                    }
                    if (!patch.is_null()) {
                        auto currentResult = detail::parseJson(result);
                        utilxx_base::Json next
                            = (hasResult && currentResult.ok)
                                  ? currentResult.value
                                  : utilxx_base::Json(nullptr);
                        detail::mergeJsonPatch(next, patch);
                        result    = next.dump();
                        hasResult = true;
                    }
                    break;
                }
            }
            startNext();
        }
    };

    DomainHooks*                        hooks_        = nullptr;
    std::shared_ptr<CapabilityRegistry> capabilities_ = std::make_shared<CapabilityRegistry>();
    std::shared_ptr<BroadcastRegistry>  broadcasts_   = std::make_shared<BroadcastRegistry>();
};

} // namespace pluginxx
