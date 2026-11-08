/// pluginxx 通用表入口实现 (主程序侧 vtable 的通用部分, 与主程序业务无关)
///
/// 用法 (主程序侧 `query_interface` 实现):
/// ```cpp
/// const void* PLUGINXX_CALL xx_query_interface(
///     const PluginxxHost*, const PluginxxStringView* iid) {
///     ...
///     if (n == "__vtable") return &hostVtable();
///     if (const void* t = pluginxx::queryGenericPluginIface<MyInstance, MyManager>(n)) return t;
///     // 业务表 (tools / permission / hooks / session / model / prompt / resources / graph)
///     ...
/// }
/// ```
///
/// 覆盖 10 张通用表: log / json / config / plugins / broadcasts / scheduler /
/// coroutine_runtime / tasks / cancel / capabilities。入口体只做三件事:
/// 解析框架控制块 (取实例/管理器 + admission lease) → 投递到 IO 线程 → 调用
/// [PluginHostCore] 的同名方法; 业务数据一律由 [DomainHooks] 提供。
///
/// 线程约定: 入口可在任意线程调用 (内部投递并同步等待); 表结构体首次查询时构造,
/// 之后只读, 因此可安全被多个插件线程并发查询。
#pragma once

#include "pluginxx/api/tables.h"
#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/host_core.h"
#include "pluginxx/runtime/driver.h"
#include "pluginxx/runtime/instance_base.h"
#include "pluginxx/runtime/manager_base.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "fmt/format.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace pluginxx {
namespace detail {

/// 注册/写入类入口的公共骨架: 解析 host 上下文 → 把业务逻辑投递到 IO 线程执行。
///
/// - 参数视图只在本次调用内有效, 因此闭包必须按值捕获自己需要的拷贝;
/// - `fn` 的入参是实例与管理器 (投递期间由 `keep` 保活, 含 admission lease);
///   业务参数校验由 `fn` 自己完成, 失败返回非 0 状态码。
template<typename InstanceT, typename ManagerT, typename Fn>
int32_t onInstanceIo(const PluginxxHost* host, Fn&& fn) {
    return guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterPluginHost<InstanceT, ManagerT>(host);
        if (!call.ok()) {
            return -1;
        }
        auto keep = call; // 投递期间持实例/管理器强引用与 admission lease
        return ioCallSyncKeep<int32_t>(
            keep,
            keep.manager(),
            [keep, fn = std::forward<Fn>(fn)]() -> int32_t {
                return fn(keep.instance(), keep.manager());
            }
        );
    });
}

/// 只读查询类入口的公共骨架 (允许关闭中查询):
/// 在 IO 线程取字符串结果 → 经 host->alloc 写入 `out`; 结果为空按失败返回 -1。
template<typename InstanceT, typename ManagerT, typename Fn>
int32_t queryStringIo(const PluginxxHost* host, PluginxxString* out, Fn&& fn) {
    if (!out) {
        return -1;
    }
    return guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterPluginHost<InstanceT, ManagerT>(host, /*allowClosing=*/true);
        if (!call.ok()) {
            return -1;
        }
        auto keep = call;
        auto text = ioCallSyncKeep<std::string>(
            keep,
            keep.manager(),
            [keep, fn = std::forward<Fn>(fn)]() -> std::string {
                return fn(keep.instance(), keep.manager());
            }
        );
        if (text.empty()) {
            return -1;
        }
        hostMemorySetString(out, text);
        return 0;
    });
}

} // namespace detail

/// 通用表入口集合。
///
/// `InstanceT` 须继承 [PluginInstanceBase]; `ManagerT` 须继承
/// [PluginHostCore<InstanceT>] (入口按同名方法调用, 由编译期约束保证形态正确)。
template<typename InstanceT, typename ManagerT>
struct GenericTableEntries {
    using HostCall = PluginHostCall<InstanceT, ManagerT>;

    static HostCall enterHost(const PluginxxHost* host, bool allowClosing = false) {
        return enterPluginHost<InstanceT, ManagerT>(host, allowClosing);
    }

    // =====================================================================
    // log 表
    // =====================================================================

    static void PLUGINXX_CALL
        logEntry(const PluginxxHost* host, int32_t level, const PluginxxStringView* msg) {
        using utilxx_base::LogLevel;
        LogLevel lv = LogLevel::Info;
        switch (level) {
            case 0:
                lv = LogLevel::Trace;
                break;
            case 1:
                lv = LogLevel::Debug;
                break;
            case 2:
                lv = LogLevel::Info;
                break;
            case 3:
                lv = LogLevel::Warn;
                break;
            case 4:
                lv = LogLevel::Error;
                break;
            default:
                break;
        }
        // 模块名 `plugin.<插件名>` (计划 OBS-5): 主程序可按插件前缀单独调日志级别
        // (如 `plugin.example_plugin=debug`); 只读查询入口, 关闭过程中也允许记录
        std::string module{"plugin"};
        if (auto call = enterHost(host, /*allowClosing=*/true); call.ok()) {
            if (const auto* inst = call.instance(); inst != nullptr && !inst->name.empty()) {
                module += ".";
                module += inst->name;
            }
        }
        utilxx_base::xxLogPrint(lv, module, svToStr(msg));
    }

    // =====================================================================
    // json 表
    // =====================================================================

    static int32_t PLUGINXX_CALL jsonGetStringEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* json,
        const PluginxxStringView* key,
        PluginxxString*           out
    ) {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto inst = call.instance();
        if (!inst || detail::abiViewEmpty(json) || detail::abiViewEmpty(key)) {
            return -1;
        }
        try {
            auto j = utilxx_base::Json::parse(svToStr(*json));
            const auto keyStr = svToStr(*key);
            if (j.is_object() && j.contains(keyStr) && j[keyStr].is_string()) {
                hostMemorySetString(out, j[keyStr].get<std::string>());
                return 0;
            }
        } catch (...) {
        }
        return -1;
    }

    static int32_t PLUGINXX_CALL jsonEscapeEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* s,
        PluginxxString*           out
    ) {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto inst = call.instance();
        if (!inst || detail::abiViewEmpty(s)) {
            return -1;
        }
        const auto src = svToSv(*s);
        std::string strOut;
        strOut.reserve(src.size() + 2);
        strOut += '"';
        for (const char raw : src) {
            const auto c = static_cast<unsigned char>(raw);
            switch (c) {
                case '"':
                    strOut += "\\\"";
                    break;
                case '\\':
                    strOut += "\\\\";
                    break;
                case '\b':
                    strOut += "\\b";
                    break;
                case '\f':
                    strOut += "\\f";
                    break;
                case '\n':
                    strOut += "\\n";
                    break;
                case '\r':
                    strOut += "\\r";
                    break;
                case '\t':
                    strOut += "\\t";
                    break;
                default:
                    if (c < 0x20) {
                        strOut += fmt::format("\\u{:04x}", c);
                    } else {
                        strOut += raw;
                    }
                    break;
            }
        }
        strOut += '"';
        hostMemorySetString(out, strOut);
        return 0;
    }

    // =====================================================================
    // config 表
    // =====================================================================

    static int32_t PLUGINXX_CALL
        configGetEntry(const PluginxxHost* host, PluginxxString* out) {
        return detail::queryStringIo<InstanceT, ManagerT>(
            host,
            out,
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                auto* hooks = mgr->domainHooks();
                (void)inst;
                return hooks ? hooks->configJson() : std::string{};
            }
        );
    }

    static int32_t PLUGINXX_CALL
        pluginArgsEntry(const PluginxxHost* host, PluginxxString* out) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst) {
                return -1;
            }
            auto keep = call;
            auto json = ioCallSyncKeep<std::string>(keep, mgr, [keep]() -> std::string {
                return keep.instance()->args.is_object() ? keep.instance()->args.dump()
                                                         : std::string{"{}"};
            });
            hostMemorySetString(out, json);
            return 0;
        });
    }


    static int32_t PLUGINXX_CALL
        pluginConfigPathEntry(const PluginxxHost* host, PluginxxString* out) {
        return detail::queryStringIo<InstanceT, ManagerT>(
            host,
            out,
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)mgr;
                return inst ? inst->configPath : std::string{};
            }
        );
    }

    static int32_t PLUGINXX_CALL
        languageGetEntry(const PluginxxHost* host, PluginxxString* out) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            if (!mgr) {
                return -1;
            }
            auto keep = call;
            auto lang = ioCallSyncKeep<std::string>(keep, mgr, [keep]() {
                auto* hooks = keep.manager()->domainHooks();
                return hooks ? hooks->language() : std::string{};
            });
            if (lang.empty()) {
                lang = "en";
            }
            hostMemorySetString(out, lang);
            return 0;
        });
    }

    static int32_t PLUGINXX_CALL languageSetEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* language
    ) {
        return guardVtableCall(-1, [&]() -> int32_t {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            if (!mgr) {
                return -1;
            }
            auto              keep = call;
            const std::string lang = language ? svToStr(*language) : std::string{};
            ioCallSyncVoidKeep(keep, mgr, [keep, lang]() {
                if (auto* hooks = keep.manager()->domainHooks()) {
                    hooks->setLanguage(lang);
                }
            });
            return 0;
        });
    }

    // =====================================================================
    // plugins 表
    // =====================================================================

    static int32_t PLUGINXX_CALL
        listPluginsEntry(const PluginxxHost* host, PluginxxString* out) {
        return detail::queryStringIo<InstanceT, ManagerT>(
            host,
            out,
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                auto* hooks = mgr->domainHooks();
                return hooks ? hooks->pluginsJson() : std::string{};
            }
        );
    }

    static int32_t PLUGINXX_CALL getPluginEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* name,
        PluginxxString*           out
    ) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            if (!mgr || detail::abiViewEmpty(name)) {
                return -1;
            }
            auto              keep      = call;
            const std::string pluginName = svToStr(*name);
            auto              json      = ioCallSyncKeep<std::string>(keep, mgr, [keep, pluginName]() {
                auto* hooks = keep.manager()->domainHooks();
                return hooks ? hooks->pluginJson(pluginName) : std::string{};
            });
            if (json.empty()) {
                return -1;
            }
            hostMemorySetString(out, json);
            return 0;
        });
    }

    static int32_t PLUGINXX_CALL
        ownInfoEntry(const PluginxxHost* host, PluginxxString* out) {
        return guardVtableCall(-1, [&]() -> int32_t {
            if (!out) {
                return -1;
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst) {
                return -1;
            }
            auto              keep     = call;
            const std::string ownName  = inst->name;
            auto              json     = ioCallSyncKeep<std::string>(keep, mgr, [keep, ownName]() {
                auto* hooks = keep.manager()->domainHooks();
                return hooks ? hooks->pluginJson(ownName) : std::string{};
            });
            if (json.empty()) {
                return -1;
            }
            hostMemorySetString(out, json);
            return 0;
        });
    }

    // =====================================================================
    // broadcasts 表
    // =====================================================================

    static int32_t PLUGINXX_CALL broadcastDeclareEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* decl_json,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(decl_json)) {
            detail::setErrorString(error_out, "declare_broadcast: missing declaration");
            return PLUGINXX_RESULT_ERR_ARG;
        }
        const std::string decl = svToStr(*decl_json);
        return detail::onInstanceIo<InstanceT, ManagerT>(
            host,
            [decl, error_out](InstanceT* inst, ManagerT* mgr) {
                std::string reason;
                const int32_t rc = mgr->declareBroadcast(inst, decl, &reason);
                if (rc != PLUGINXX_RESULT_OK) {
                    detail::setErrorString(error_out, reason);
                }
                return rc;
            }
        );
    }

    static int32_t PLUGINXX_CALL broadcastPublishEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* broadcast_id,
        const PluginxxStringView* payload_json,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(broadcast_id)) {
            detail::setErrorString(error_out, "publish_broadcast: missing id");
            return PLUGINXX_RESULT_ERR_ARG;
        }
        const std::string id      = svToStr(*broadcast_id);
        const std::string payload = payload_json ? svToStr(*payload_json) : std::string{"{}"};
        return detail::onInstanceIo<InstanceT, ManagerT>(
            host,
            [id, payload, error_out](InstanceT* inst, ManagerT* mgr) {
                std::string reason;
                const int32_t rc = mgr->publishBroadcastFrom(inst, id, payload, &reason);
                if (rc != PLUGINXX_RESULT_OK) {
                    detail::setErrorString(error_out, reason);
                }
                return rc;
            }
        );
    }

    static PluginxxSubscription* PLUGINXX_CALL broadcastSubscribeEntry(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* broadcast_id,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payload_json, void* ud),
        void*                     ud,
        PluginxxString*           error_out
    ) {
        return guardVtableCall<PluginxxSubscription*>(nullptr, [&]() {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || detail::abiViewEmpty(broadcast_id) || !handler) {
                detail::setErrorString(
                    error_out,
                    "subscribe_broadcast: missing instance, id, or handler"
                );
                return static_cast<PluginxxSubscription*>(nullptr);
            }
            // 引用含借用视图: 投递前复制为自有的 id/seq (库内部结构)
            auto              keep = call;
            const std::string id   = svToStr(*broadcast_id);
            PluginxxScopeRef  scopeCopy{};
            const bool        hasScope = pluginxx_scope_ref_ok(scope) && scope;
            if (hasScope) {
                scopeCopy = *scope;
            }
            auto* sub = ioCallSyncKeep<PluginxxSubscription*>(
                keep,
                mgr,
                [keep, hasScope, scopeCopy, id, handler, ud]() {
                    return keep.manager()->subscribeBroadcast(
                        keep.instance(),
                        hasScope ? &scopeCopy : nullptr,
                        id,
                        handler,
                        ud
                    );
                }
            );
            if (!sub) {
                detail::setErrorString(error_out, "subscribe_broadcast: rejected");
            }
            return sub;
        });
    }

    static void PLUGINXX_CALL broadcastUnsubscribeEntry(PluginxxSubscription* sub) {
        guardVtableCallVoid([&] { unsubscribePluginBroadcast(sub); });
    }

    static int32_t PLUGINXX_CALL broadcastReadEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* broadcast_id,
        PluginxxString*           out_json
    ) {
        if (!out_json || detail::abiViewEmpty(broadcast_id)) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        return guardVtableCall<int32_t>(PLUGINXX_RESULT_ERR_ARG, [&]() -> int32_t {
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            if (!mgr) {
                return PLUGINXX_RESULT_ERR_ARG;
            }
            const std::string id = svToStr(*broadcast_id);
            return mgr->readBroadcast(id, out_json);
        });
    }

    // =====================================================================
    // capabilities 表
    // =====================================================================

    static int32_t PLUGINXX_CALL capabilityRegisterEntry(
        const PluginxxHost*           host,
        const PluginxxScopeRef*       scope,
        const PluginxxCapabilitySpec* spec,
        PluginxxString*               error_out
    ) {
        if (!spec || spec->struct_size < sizeof(PluginxxCapabilitySpec)) {
            detail::setErrorString(
                error_out,
                "register_capability: spec is missing or too short"
            );
            return PLUGINXX_RESULT_ERR_ARG;
        }
        // 视图只在本次调用内有效: 复制成自有字符串后再投递
        const std::string id       = svToStr(spec->id);
        const std::string ownerTag = svToStr(spec->owner_tag);
        PluginxxCapabilitySpec local = *spec;
        PluginxxScopeRef       scopeCopy{};
        const bool             hasScope = pluginxx_scope_ref_ok(scope) && scope;
        if (hasScope) {
            scopeCopy = *scope;
        }
        return detail::onInstanceIo<InstanceT, ManagerT>(
            host,
            [local, id, ownerTag, hasScope, scopeCopy, error_out](
                InstanceT* inst,
                ManagerT*  mgr
            ) {
                PluginxxCapabilitySpec resolved = local;
                resolved.id        = PluginxxStringView{id.data(), id.size()};
                resolved.owner_tag = PluginxxStringView{ownerTag.data(), ownerTag.size()};
                std::string reason;
                const int32_t rc = mgr->registerCapability(
                    inst,
                    hasScope ? &scopeCopy : nullptr,
                    resolved,
                    &reason
                );
                if (rc != PLUGINXX_RESULT_OK) {
                    detail::setErrorString(error_out, reason);
                }
                return rc;
            }
        );
    }

    static int32_t PLUGINXX_CALL capabilityUnregisterEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* owner_tag
    ) {
        if (detail::abiViewEmpty(capability_id)) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        const std::string id  = svToStr(*capability_id);
        const std::string tag = owner_tag ? svToStr(*owner_tag) : std::string{};
        return detail::onInstanceIo<InstanceT, ManagerT>(
            host,
            [id, tag](InstanceT* inst, ManagerT* mgr) {
                return mgr->unregisterCapability(inst, id, tag);
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL capabilityInvokeEntry(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* args_json,
        int32_t                   phase,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    ) {
        return capabilityInvokeCommon(
            host, scope, capability_id, args_json, phase, -1, cb, ud, error_out
        );
    }

    /// `invoke_capability_ex` (追加成员): 多一个本次调用的超时
    static PluginxxOperatorHandle* PLUGINXX_CALL capabilityInvokeExEntry(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* args_json,
        int32_t                   phase,
        int32_t                   timeout_ms,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    ) {
        return capabilityInvokeCommon(
            host, scope, capability_id, args_json, phase, timeout_ms, cb, ud, error_out
        );
    }

    /// 两条调用入口共用的实现 (`timeout_ms < 0` = 用声明里的超时)
    static PluginxxOperatorHandle* capabilityInvokeCommon(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* args_json,
        int32_t                   phase,
        int32_t                   timeout_ms,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    ) {
        return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || detail::abiViewEmpty(capability_id)) {
                return static_cast<PluginxxOperatorHandle*>(nullptr);
            }
            // 跨边界视图只在本次调用内有效: 投递前复制为自有字符串
            auto              keep = call;
            const std::string id   = svToStr(*capability_id);
            const std::string args = args_json ? svToStr(*args_json) : std::string{"{}"};
            PluginxxScopeRef  scopeCopy{};
            const bool        hasScope = pluginxx_scope_ref_ok(scope) && scope;
            if (hasScope) {
                scopeCopy = *scope;
            }
            return ioCallSyncKeep<PluginxxOperatorHandle*>(
                keep,
                mgr,
                [keep, hasScope, scopeCopy, id, args, phase, timeout_ms, cb, ud, error_out]() {
                    return keep.manager()->invokeCapabilityAsync(
                        keep.instance(),
                        hasScope ? &scopeCopy : nullptr,
                        id,
                        args,
                        phase,
                        timeout_ms,
                        cb,
                        ud,
                        error_out
                    );
                }
            );
        });
    }

    static int32_t PLUGINXX_CALL capabilityInfoEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* capability_id,
        PluginxxString*           out_json
    ) {
        if (!out_json || detail::abiViewEmpty(capability_id)) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        const std::string id = svToStr(*capability_id);
        return detail::onInstanceIo<InstanceT, ManagerT>(
            host,
            [id, out_json](InstanceT*, ManagerT* mgr) {
                return mgr->capabilityInfo(id, out_json);
            }
        );
    }

    static int32_t PLUGINXX_CALL capabilityListEntry(
        const PluginxxHost* host,
        PluginxxString*     out_json
    ) {
        if (!out_json) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        return detail::onInstanceIo<InstanceT, ManagerT>(
            host,
            [out_json](InstanceT*, ManagerT* mgr) {
                return mgr->listCapabilities(out_json);
            }
        );
    }

    // =====================================================================
    // scheduler 表
    // =====================================================================

    static int32_t PLUGINXX_CALL isIoThreadEntry(const PluginxxHost* host) {
        // 只读查询：关闭过程中仍允许回答（返回 0 表示"不是 IO 线程"）
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        return (mgr && mgr->isIoThread()) ? 1 : 0;
    }

    static int32_t PLUGINXX_CALL
        postToIoEntry(const PluginxxHost* host, void(PLUGINXX_CALL* fn)(void*), void* ud) {
        return guardVtableCall(-1, [&]() -> int32_t {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !fn) {
                return -1;
            }
            auto keep = call;
            return ioCallSyncKeep<int32_t>(keep, mgr, [keep, fn, ud]() -> int32_t {
                return keep.manager()->postCallback(keep.instance(), fn, ud) ? 0 : -1;
            });
        });
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL sleepEntry(
        const PluginxxHost*      host,
        int64_t                       ms,
        PluginxxOperatorCallback cb,
        void*                         ud,
        PluginxxString*          error_out
    ) {
        return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !cb) {
                detail::setErrorString(error_out, "scheduler sleep: plugin runtime unavailable");
                return static_cast<PluginxxOperatorHandle*>(nullptr);
            }
            // sleep 自身会经 OpCore 获取实例执行 lease, 这里只需投递到 IO 线程
            auto keep = call;
            return ioCallSyncKeep<PluginxxOperatorHandle*>(
                keep,
                mgr,
                [keep, ms, cb, ud, error_out]() {
                    return keep.manager()->sleep(keep.instance(), ms, cb, ud, error_out);
                }
            );
        });
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL offloadEntry(
        const PluginxxHost* host,
        void*(PLUGINXX_CALL* work)(
            void*                           ud,
            const PluginxxCancelToken* token,
            PluginxxString*            error_out
        ),
        void(PLUGINXX_CALL* done)(
            void*                          ud,
            int32_t                        status,
            void*                          result,
            const PluginxxStringView* error
        ),
        void*                ud,
        PluginxxString* error_out
    ) {
        return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !work) {
                detail::setErrorString(error_out, "scheduler offload: plugin runtime unavailable");
                return static_cast<PluginxxOperatorHandle*>(nullptr);
            }
            auto keep = call;
            return ioCallSyncKeep<PluginxxOperatorHandle*>(
                keep,
                mgr,
                [keep, work, done, ud, error_out]() {
                    return keep.manager()->offload(keep.instance(), work, done, ud, error_out);
                }
            );
        });
    }

    /// Operation 取消 (scheduler / capabilities / tasks 三张表共用)
    static void PLUGINXX_CALL opCancelEntry(PluginxxOperatorHandle* op) {
        cancelPluginOperation(op);
    }

    // =====================================================================
    // coroutine_runtime 表
    // =====================================================================

    /// 申请一次驱动请求 (任意线程可调用; 永不内联回调)
    ///
    /// 语义见 pluginxx/api/tables.h 的接口表声明与 runtime/driver.h 的实现说明:
    /// - admission 采用 `allowClosing=true` 的 lifecycle lease: 实例进入 Closing
    ///   后仍必须允许驱动, 否则"取消全部 Operation → 插件收束 root"会因为拿不到
    ///   驱动而永远无法跑完;
    /// - Disabled/Closed 拒绝 (lease 获取失败);
    /// - 入队失败 (IO executor 不可用/已停止) 返回 NULL, 调用方必须把受影响的操作
    ///   以失败终结, 并且请求此时已经收束 (不会泄漏 lease)。
    static PluginxxDriver* PLUGINXX_CALL requestDriverEntry(
        const PluginxxHost*   host,
        PluginxxDriveOnceFn   drive_once,
        void*                      user_data,
        PluginxxString*       error_out
    ) {
        return guardVtableCall<PluginxxDriver*>(nullptr, [&]() {
            if (!drive_once) {
                detail::setErrorString(error_out, "coroutine runtime: null drive callback");
                return static_cast<PluginxxDriver*>(nullptr);
            }
            auto call = enterHost(host, /*allowClosing=*/true);
            auto inst = call.instance();
            auto mgr  = call.manager();
            if (!mgr || !inst || !inst->lifetime) {
                detail::setErrorString(
                    error_out,
                    "coroutine runtime: plugin instance is closed or unavailable"
                );
                return static_cast<PluginxxDriver*>(nullptr);
            }
            auto driver = PluginxxDriver::create(
                mgr->runtime(),
                inst->lifetime,
                drive_once,
                user_data,
                inst->name + " driver"
            );
            if (!driver) {
                detail::setErrorString(
                    error_out,
                    "coroutine runtime: plugin instance is closing or closed"
                );
                return static_cast<PluginxxDriver*>(nullptr);
            }
            // 句柄墓碑先登记再排队: 迟到 cancel_driver 只会观察终态, 不会解引用已释放对象。
            inst->retainDriverHandle(driver);
            if (!driver->schedule()) {
                detail::setErrorString(error_out, "coroutine runtime: host IO executor is unavailable");
                return static_cast<PluginxxDriver*>(nullptr);
            }
            return driver.get();
        });
    }

    /// 取消尚未开始的请求 (幂等, 非阻塞, 任意线程可调用)
    ///
    /// `cancel_driver` 的 ABI 形态不含 host 参数, 因此句柄校验走请求自身的进程级
    /// 地址注册表: 命中才解引用 (weak_ptr 升级为强引用), 伪造/过期指针安全忽略。
    static void PLUGINXX_CALL cancelDriverEntry(PluginxxDriver* driver) {
        if (!driver) {
            return;
        }
        guardVtableCallVoid([&] {
            if (!PluginxxDriver::cancelByHandle(driver)) {
                XX_LOGW("Plugin driver cancellation ignored: handle is not an active ticket");
            }
        });
    }

    // =====================================================================
    // tasks 表
    // =====================================================================

    static PluginxxOperatorHandle* PLUGINXX_CALL registerTaskEntry(
        const PluginxxHost*            host,
        PluginxxOperatorCancelFunction cancel_fn,
        void*                               cancel_ud,
        PluginxxOperatorNotify*        notify,
        PluginxxString*                error_out
    ) {
        return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !notify) {
                return static_cast<PluginxxOperatorHandle*>(nullptr);
            }
            auto keep = call;
            return ioCallSyncKeep<PluginxxOperatorHandle*>(
                keep,
                mgr,
                [keep, cancel_fn, cancel_ud, notify, error_out]() {
                    return keep.manager()
                        ->registerTask(keep.instance(), cancel_fn, cancel_ud, notify, error_out);
                }
            );
        });
    }

    // =====================================================================
    // cancel 表
    // =====================================================================


    // =====================================================================
    // 表结构体 (首次查询时构造; 线程安全的静态初始化)
    // =====================================================================

    static const PluginxxLogIface& logIface() {
        static const PluginxxLogIface iface{
            PLUGINXX_IFACE_LOG_VERSION,
            sizeof(PluginxxLogIface),
            &logEntry,
        };
        return iface;
    }

    static const PluginxxJsonIface& jsonIface() {
        static const PluginxxJsonIface iface{
            PLUGINXX_IFACE_JSON_VERSION,
            sizeof(PluginxxJsonIface),
            &jsonGetStringEntry,
            &jsonEscapeEntry,
        };
        return iface;
    }

    static const PluginxxConfigIface& configIface() {
        static const PluginxxConfigIface iface{
            PLUGINXX_IFACE_CONFIG_VERSION,
            sizeof(PluginxxConfigIface),
            &configGetEntry,
            &pluginArgsEntry,
            &pluginConfigPathEntry,
            &languageGetEntry,
            &languageSetEntry,
        };
        return iface;
    }

    static const PluginxxPluginsIface& pluginsIface() {
        static const PluginxxPluginsIface iface{
            PLUGINXX_IFACE_PLUGINS_VERSION,
            sizeof(PluginxxPluginsIface),
            &listPluginsEntry,
            &getPluginEntry,
            &ownInfoEntry,
        };
        return iface;
    }

    static const PluginxxBroadcastsIface& broadcastsIface() {
        static const PluginxxBroadcastsIface iface{
            PLUGINXX_IFACE_BROADCASTS_VERSION,
            sizeof(PluginxxBroadcastsIface),
            &broadcastDeclareEntry,
            &broadcastPublishEntry,
            &broadcastSubscribeEntry,
            &broadcastReadEntry,
            &broadcastUnsubscribeEntry,
        };
        return iface;
    }

    static const PluginxxCapabilitiesIface& capabilitiesIface() {
        static const PluginxxCapabilitiesIface iface{
            PLUGINXX_IFACE_CAPABILITIES_VERSION,
            sizeof(PluginxxCapabilitiesIface),
            &capabilityRegisterEntry,
            &capabilityUnregisterEntry,
            &capabilityInvokeEntry,
            &capabilityInfoEntry,
            &capabilityListEntry,
            &opCancelEntry,
            &capabilityInvokeExEntry,
        };
        return iface;
    }

    static const PluginxxSchedulerIface& schedulerIface() {
        static const PluginxxSchedulerIface iface{
            PLUGINXX_IFACE_SCHEDULER_VERSION,
            sizeof(PluginxxSchedulerIface),
            &isIoThreadEntry,
            &postToIoEntry,
            &sleepEntry,
            &opCancelEntry,
            &offloadEntry,
        };
        return iface;
    }

    static const PluginxxCoroutineRuntimeIface& coroutineRuntimeIface() {
        static const PluginxxCoroutineRuntimeIface iface{
            PLUGINXX_IFACE_COROUTINE_RUNTIME_VERSION,
            sizeof(PluginxxCoroutineRuntimeIface),
            &requestDriverEntry,
            &cancelDriverEntry,
            &isIoThreadEntry,
        };
        return iface;
    }

    static const PluginxxTasksIface& tasksIface() {
        static const PluginxxTasksIface iface{
            PLUGINXX_IFACE_TASKS_VERSION,
            sizeof(PluginxxTasksIface),
            &registerTaskEntry,
            &opCancelEntry,
        };
        return iface;
    }

    static const PluginxxCancelIface& cancelIface() {
        static const PluginxxCancelIface iface{
            PLUGINXX_IFACE_CANCEL_VERSION,
            sizeof(PluginxxCancelIface),
            &opCancelEntry,
        };
        return iface;
    }
};

/// 通用表的表名清单 (IID 里 `<应用名>.` 之后那一段): 应用名不一致时用来给提示
inline constexpr std::string_view kGenericTableNames[] = {
    "log",         "json",     "config",    "plugins",    "broadcasts",
    "capabilities", "scheduler", "coroutine_runtime", "tasks", "cancel",
};

/// `iid` 是否以 `.<已知通用表名>` 结尾
///
/// 只做后缀比较, 不按点切分 (应用名 / 表名都可以含点)。
inline bool looksLikeGenericTableIid(std::string_view iid) {
    for (const auto name : kGenericTableNames) {
        if (iid.size() <= name.size() + 1) {
            continue;
        }
        const auto suffixAt = iid.size() - name.size();
        if (iid[suffixAt - 1] == '.' && iid.compare(suffixAt, name.size(), name) == 0) {
            return true;
        }
    }
    return false;
}

/// 按 IID 查询**通用表** (未命中返回 nullptr, 主程序继续查自己的业务表)
/// - 覆盖 10 张通用表; IID 为编译期常量字符串, 比较用 string_view (不构造临时串)
/// - 未命中但形如 `<别的应用名>.<通用表名>` 时记一条日志: 宿主与插件必须用同一个
///   应用名 (`PLUGINXX_APP_NAME`) 编译, 否则插件拿不到表 (只提示, 不改行为)
template<typename InstanceT, typename ManagerT>
const void* queryGenericPluginIface(std::string_view iid) {
    using Entries = GenericTableEntries<InstanceT, ManagerT>;
    if (iid == PLUGINXX_IFACE_LOG) {
        return &Entries::logIface();
    }
    if (iid == PLUGINXX_IFACE_JSON) {
        return &Entries::jsonIface();
    }
    if (iid == PLUGINXX_IFACE_CONFIG) {
        return &Entries::configIface();
    }
    if (iid == PLUGINXX_IFACE_PLUGINS) {
        return &Entries::pluginsIface();
    }
    if (iid == PLUGINXX_IFACE_BROADCASTS) {
        return &Entries::broadcastsIface();
    }
    if (iid == PLUGINXX_IFACE_CAPABILITIES) {
        return &Entries::capabilitiesIface();
    }
    if (iid == PLUGINXX_IFACE_SCHEDULER) {
        return &Entries::schedulerIface();
    }
    if (iid == PLUGINXX_IFACE_COROUTINE_RUNTIME) {
        return &Entries::coroutineRuntimeIface();
    }
    if (iid == PLUGINXX_IFACE_TASKS) {
        return &Entries::tasksIface();
    }
    if (iid == PLUGINXX_IFACE_CANCEL) {
        return &Entries::cancelIface();
    }
    if (looksLikeGenericTableIid(iid)) {
        utilxx_base::xxLogPrint(
            utilxx_base::LogLevel::Warn,
            "pluginxx",
            "query_interface: `" + std::string{iid} + "` 未命中; 本进程的通用表 IID 以 `"
                + std::string{pluginxx::appIdPrefix()}
                + "` 开头 —— 宿主与插件必须用同一个应用名 (PLUGINXX_APP_NAME) 编译"
        );
    }
    return nullptr;
}

} // namespace pluginxx
