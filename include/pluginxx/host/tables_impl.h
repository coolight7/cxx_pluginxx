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
/// 覆盖 9 张通用表: log / json / config / plugins / broadcasts / scheduler /
/// tasks / cancel / capabilities。入口体只做三件事:
/// 解析框架控制块 (取实例/管理器 + admission lease) → 投递到 IO 线程 → 调用
/// [PluginHostCore] 的同名方法; 业务数据一律由 [DomainServices] 提供。
///
/// 线程约定: 入口可在任意线程调用。注册/写入类入口投递到 IO 线程并同步等待;
/// 四个“提交即返回”的入口 (`capabilities.invoke_*` / `scheduler.sleep` /
/// `scheduler.offload` / `tasks.register_task`) 在**调用线程受理** (句柄立即可用),
/// 需要 IO 线程状态的工作体随后在 IO 线程执行。表结构体首次查询时构造,
/// 之后只读, 因此可安全被多个插件线程并发查询。
#pragma once

#include "pluginxx/api/kinds.h"
#include "pluginxx/api/tables.h"
#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/host_core.h"
#include "pluginxx/host/table_op.h"
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

/// 异步表调用的公共骨架 (与主程序业务表同一套契约, 见 pluginxx/host/table_op.h)
///
/// - 返回非 NULL = 已受理 (结果经 `cb` 恰好一次在 IO 线程交回); 返回 NULL = 未受理
///   (参数形状错 / 实例不可用), 原因写 `error_out`;
/// - 工作体在 IO 线程执行, 返回结果正文; 抛异常 = 本次失败, 正文是原因;
/// - `allowClosing=true` 用于只读查询 (实例收尾时其工具仍在跑, 仍可查)。
template<typename InstanceT, typename ManagerT, typename Fn>
PluginxxOperatorHandle* asyncTableOp(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work,
    bool                     allowClosing = false
) {
    auto call = enterPluginHost<InstanceT, ManagerT>(host, allowClosing);
    if (!call.ok()) {
        setTableOpError(error_out, "plugin runtime is unavailable");
        return nullptr;
    }
    auto keep = call;
    return startTableOp(
        keep.manager(),
        cb,
        ud,
        error_out,
        std::move(label),
        [keep, work = std::forward<Fn>(work)]() mutable -> std::string {
            return work(keep.instance(), keep.manager());
        },
        keep.inst
    );
}

/// 只读查询类异步入口: 空结果按 `not_found` 交回 (与旧版"空串 = 失败"同义)
template<typename InstanceT, typename ManagerT, typename Fn>
PluginxxOperatorHandle* asyncQueryOp(
    const PluginxxHost*      host,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work
) {
    return asyncTableOp<InstanceT, ManagerT>(
        host,
        cb,
        ud,
        error_out,
        std::move(label),
        [work = std::forward<Fn>(work)](InstanceT* inst, ManagerT* mgr) mutable -> std::string {
            auto text = work(inst, mgr);
            if (text.empty()) {
                throw TableOpError("not_found");
            }
            return text;
        },
        /*allowClosing=*/true
    );
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

    static PluginxxOperatorHandle* PLUGINXX_CALL configGetEntry(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    ) {
        return detail::asyncQueryOp<InstanceT, ManagerT>(
            host,
            cb,
            ud,
            error_out,
            "config.get_config",
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                auto* services = mgr->domainServices();
                return services ? services->configJson() : std::string{};
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL pluginArgsEntry(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    ) {
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            ud,
            error_out,
            "config.get_plugin_args",
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)mgr;
                if (inst == nullptr) {
                    throw TableOpError("not_found");
                }
                return inst->args.is_object() ? inst->args.dump() : std::string{"{}"};
            },
            /*allowClosing=*/true
        );
    }


    static PluginxxOperatorHandle* PLUGINXX_CALL pluginConfigPathEntry(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    ) {
        return detail::asyncQueryOp<InstanceT, ManagerT>(
            host,
            cb,
            ud,
            error_out,
            "config.get_plugin_config_path",
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)mgr;
                return inst ? inst->configPath : std::string{};
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL languageGetEntry(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    ) {
        return detail::asyncQueryOp<InstanceT, ManagerT>(
            host,
            cb,
            ud,
            error_out,
            "config.get_language",
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                auto* services = mgr->domainServices();
                auto  lang     = services ? services->language() : std::string{};
                return lang.empty() ? std::string{"en"} : lang;
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL languageSetEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* language,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    ) {
        const std::string lang = language ? svToStr(*language) : std::string{};
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            ud,
            error_out,
            "config.set_language",
            [lang](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                if (auto* services = mgr->domainServices()) {
                    services->setLanguage(lang);
                }
                return std::string{};
            }
        );
    }

    // =====================================================================
    // plugins 表
    // =====================================================================

    static PluginxxOperatorHandle* PLUGINXX_CALL listPluginsEntry(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    ) {
        return detail::asyncQueryOp<InstanceT, ManagerT>(
            host,
            cb,
            ud,
            error_out,
            "plugins.list_plugins",
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                auto* services = mgr->domainServices();
                return services ? services->pluginsJson() : std::string{};
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL getPluginEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* name,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(name)) {
            setTableOpError(error_out, "get_plugin: plugin name is required");
            return nullptr;
        }
        const std::string pluginName = svToStr(*name);
        return detail::asyncQueryOp<InstanceT, ManagerT>(
            host,
            cb,
            ud,
            error_out,
            "plugins.get_plugin",
            [pluginName](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                auto* services = mgr->domainServices();
                return services ? services->pluginJson(pluginName) : std::string{};
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL ownInfoEntry(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    ) {
        return detail::asyncQueryOp<InstanceT, ManagerT>(
            host,
            cb,
            ud,
            error_out,
            "plugins.get_own_info",
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                auto* services = mgr->domainServices();
                if (inst == nullptr || services == nullptr) {
                    return std::string{};
                }
                return services->pluginJson(inst->name);
            }
        );
    }

    // =====================================================================
    // broadcasts 表
    // =====================================================================

    static PluginxxOperatorHandle* PLUGINXX_CALL broadcastDeclareEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* decl_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(decl_json)) {
            setTableOpError(error_out, "declare_broadcast: missing declaration");
            return nullptr;
        }
        const std::string decl = svToStr(*decl_json);
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "broadcasts.declare_broadcast",
            [decl](InstanceT* inst, ManagerT* mgr) -> std::string {
                std::string   reason;
                const int32_t rc = mgr->declareBroadcast(inst, decl, &reason);
                if (rc != PLUGINXX_RESULT_OK) {
                    throw TableOpError(
                        reason.empty() ? std::string{"declare_broadcast failed"}
                                       : std::move(reason)
                    );
                }
                return std::string{};
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL broadcastPublishEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* broadcast_id,
        const PluginxxStringView* payload_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(broadcast_id)) {
            setTableOpError(error_out, "publish_broadcast: missing id");
            return nullptr;
        }
        const std::string id      = svToStr(*broadcast_id);
        const std::string payload = payload_json ? svToStr(*payload_json) : std::string{"{}"};
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "broadcasts.publish_broadcast",
            [id, payload](InstanceT* inst, ManagerT* mgr) -> std::string {
                std::string   reason;
                const int32_t rc = mgr->publishBroadcastFrom(inst, id, payload, &reason);
                if (rc != PLUGINXX_RESULT_OK) {
                    throw TableOpError(
                        reason.empty() ? std::string{"publish_broadcast failed"}
                                       : std::move(reason)
                    );
                }
                return std::string{};
            }
        );
    }

    /// 订阅的公共实现 (普通订阅与带 `on_end` 的订阅共用)
    template<typename OnEnd>
    static PluginxxOperatorHandle* subscribeBroadcastOp(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* broadcast_id,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payload_json, void* ud),
        OnEnd                     on_end,
        void*                     ud,
        const PluginxxStringView* options_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out,
        const char*               label
    ) {
        if (detail::abiViewEmpty(broadcast_id) || handler == nullptr) {
            setTableOpError(error_out, fmt::format("{}: id and handler are required", label));
            return nullptr;
        }
        // 引用含借用视图: 投递前复制成自有结构 (只在本次操作期间使用)
        PluginxxScopeRef  scopeCopy{};
        const bool        hasScope = pluginxx_scope_ref_ok(scope) && scope != nullptr;
        if (hasScope) {
            scopeCopy = *scope;
        }
        const std::string id      = svToStr(*broadcast_id);
        const std::string options = (options_json && options_json->data)
                                        ? svToStr(*options_json)
                                        : std::string{};
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            label,
            [hasScope, scopeCopy, id, options, handler, on_end, ud](
                InstanceT* inst,
                ManagerT*  mgr
            ) -> std::string {
                auto* sub = mgr->subscribeBroadcast(
                    inst,
                    hasScope ? &scopeCopy : nullptr,
                    id,
                    handler,
                    ud,
                    options,
                    on_end
                );
                if (sub == nullptr) {
                    throw TableOpError("subscribe_broadcast: rejected");
                }
                return pluginxxSubscriptionToText(sub);
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL broadcastSubscribeEntry(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* broadcast_id,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payload_json, void* ud),
        void*                     ud,
        const PluginxxStringView* options_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        return subscribeBroadcastOp(
            host,
            scope,
            broadcast_id,
            handler,
            nullptr,
            ud,
            options_json,
            cb,
            cb_ud,
            error_out,
            "broadcasts.subscribe_broadcast"
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL broadcastUnsubscribeEntry(
        const PluginxxHost*      host,
        PluginxxSubscription*    sub,
        PluginxxOperatorCallback cb,
        void*                    cb_ud,
        PluginxxString*          error_out
    ) {
        if (sub == nullptr) {
            setTableOpError(error_out, "unsubscribe_broadcast: handle is required");
            return nullptr;
        }
        // 工作体本来就跑在 IO 线程上, 撤销是就地完成的 (幂等: 已撤销 / 已结束都算成功)
        // 关闭中仍允许撤销: 插件在 `stop` 里退订也要生效 (旧实现与实例状态无关)
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "broadcasts.unsubscribe_broadcast",
            [sub](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                (void)mgr;
                unsubscribePluginBroadcast(sub);
                return std::string{};
            },
            /*allowClosing=*/true
        );
    }

    /// `end_broadcast` (追加成员, v3): 属主结束一条流
    static PluginxxOperatorHandle* PLUGINXX_CALL broadcastEndEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* broadcast_id,
        const PluginxxStringView* reason_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(broadcast_id)) {
            setTableOpError(error_out, "end_broadcast: missing id");
            return nullptr;
        }
        const std::string id     = svToStr(*broadcast_id);
        const std::string reason = reason_json ? svToStr(*reason_json) : std::string{};
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "broadcasts.end_broadcast",
            [id, reason](InstanceT* inst, ManagerT* mgr) -> std::string {
                std::string   local;
                const int32_t rc = mgr->endBroadcastFrom(inst, id, reason, &local);
                if (rc != PLUGINXX_RESULT_OK) {
                    throw TableOpError(
                        local.empty() ? std::string{"end_broadcast failed"} : std::move(local)
                    );
                }
                return std::string{};
            }
        );
    }

    /// `subscribe_broadcast_ex` (追加成员, v3): 多一个结束回调
    ///
    /// 结束回调的租赁保护与普通回调一致: `on_end` 在注册表锁外、在实例执行 lease 下调用
    /// (句柄的 `alive` 标志在摧销后立即失活, 不再回调已卸载的插件)。
    static PluginxxOperatorHandle* PLUGINXX_CALL broadcastSubscribeExEntry(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* broadcast_id,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payload_json, void* ud),
        void(PLUGINXX_CALL* on_end)(const PluginxxStringView* reason_json, void* ud),
        void*                     ud,
        const PluginxxStringView* options_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        return subscribeBroadcastOp(
            host,
            scope,
            broadcast_id,
            handler,
            on_end,
            ud,
            options_json,
            cb,
            cb_ud,
            error_out,
            "broadcasts.subscribe_broadcast_ex"
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL broadcastReadEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* broadcast_id,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(broadcast_id)) {
            setTableOpError(error_out, "read_broadcast: missing id");
            return nullptr;
        }
        const std::string id = svToStr(*broadcast_id);
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "broadcasts.read_broadcast",
            [id](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                std::string   value;
                const int32_t rc = mgr->readBroadcastText(id, &value);
                if (rc != PLUGINXX_RESULT_OK) {
                    const char* name = result::nameOf(rc);
                    throw TableOpError(
                        fmt::format("read_broadcast: {}", name ? name : "failed")
                    );
                }
                return value;
            },
            /*allowClosing=*/true
        );
    }

    // =====================================================================
    // capabilities 表
    // =====================================================================

    static PluginxxOperatorHandle* PLUGINXX_CALL capabilityRegisterEntry(
        const PluginxxHost*           host,
        const PluginxxScopeRef*       scope,
        const PluginxxCapabilitySpec* spec,
        PluginxxOperatorCallback      cb,
        void*                         cb_ud,
        PluginxxString*               error_out
    ) {
        if (!spec || spec->struct_size < sizeof(PluginxxCapabilitySpec)) {
            setTableOpError(error_out, "register_capability: spec is missing or too short");
            return nullptr;
        }
        // 视图只在本次调用内有效: 复制成自有字符串后再投递
        const std::string     id       = svToStr(spec->id);
        const std::string     ownerTag = svToStr(spec->owner_tag);
        PluginxxCapabilitySpec local   = *spec;
        PluginxxScopeRef       scopeCopy{};
        const bool             hasScope = pluginxx_scope_ref_ok(scope) && scope;
        if (hasScope) {
            scopeCopy = *scope;
        }
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "capabilities.register_capability",
            [local, id, ownerTag, hasScope, scopeCopy](
                InstanceT* inst,
                ManagerT*  mgr
            ) -> std::string {
                PluginxxCapabilitySpec resolved = local;
                resolved.id        = PluginxxStringView{id.data(), id.size()};
                resolved.owner_tag = PluginxxStringView{ownerTag.data(), ownerTag.size()};
                std::string   reason;
                const int32_t rc = mgr->registerCapability(
                    inst,
                    hasScope ? &scopeCopy : nullptr,
                    resolved,
                    &reason
                );
                if (rc != PLUGINXX_RESULT_OK) {
                    const char* name = result::nameOf(rc);
                    throw TableOpError(
                        !reason.empty() ? std::move(reason)
                                        : fmt::format(
                                              "register_capability: {}",
                                              name ? name : "failed"
                                          )
                    );
                }
                return std::string{};
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL capabilityUnregisterEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* owner_tag,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(capability_id)) {
            setTableOpError(error_out, "unregister_capability: missing id");
            return nullptr;
        }
        const std::string id  = svToStr(*capability_id);
        const std::string tag = owner_tag ? svToStr(*owner_tag) : std::string{};
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "capabilities.unregister_capability",
            [id, tag](InstanceT* inst, ManagerT* mgr) -> std::string {
                const int32_t rc = mgr->unregisterCapability(inst, id, tag);
                if (rc != PLUGINXX_RESULT_OK) {
                    const char* name = result::nameOf(rc);
                    throw TableOpError(
                        fmt::format("unregister_capability: {}", name ? name : "failed")
                    );
                }
                return std::string{};
            }
        );
    }

    /// `unregister_capability_ex` (追加成员, v6): 按作用域注销一条实现行
    static PluginxxOperatorHandle* PLUGINXX_CALL capabilityUnregisterExEntry(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* owner_tag,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(capability_id)) {
            setTableOpError(error_out, "unregister_capability_ex: missing id");
            return nullptr;
        }
        const std::string id  = svToStr(*capability_id);
        const std::string tag = owner_tag ? svToStr(*owner_tag) : std::string{};
        PluginxxScopeRef  scopeCopy{};
        const bool        hasScope = pluginxx_scope_ref_ok(scope) && scope;
        if (hasScope) {
            scopeCopy = *scope;
        }
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "capabilities.unregister_capability_ex",
            [id, tag, hasScope, scopeCopy](InstanceT* inst, ManagerT* mgr) -> std::string {
                const int32_t rc = mgr->unregisterScopedCapability(
                    inst,
                    hasScope ? &scopeCopy : nullptr,
                    id,
                    tag
                );
                if (rc != PLUGINXX_RESULT_OK) {
                    const char* name = result::nameOf(rc);
                    throw TableOpError(
                        fmt::format("unregister_capability_ex: {}", name ? name : "failed")
                    );
                }
                return std::string{};
            }
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL capabilityInvokeEntry(
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

    /// 调用入口的实现 (`timeout_ms < 0` = 用声明里的超时)
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
            // 插件在 stop 事务里也要能调能力 (典型场景: 通知另一个插件卸载自己的资源);
            // 因此允许调用方实例处于关闭中 —— 租约仍由 OpCore 把关
            // (调用方租约按 callerMayStop 放宽, 被调一方仍按严格判定)
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || detail::abiViewEmpty(capability_id)) {
                return static_cast<PluginxxOperatorHandle*>(nullptr);
            }
            // 跨边界视图只在本次调用内有效: 投递前复制为自有字符串
            const std::string id   = svToStr(*capability_id);
            const std::string args = args_json ? svToStr(*args_json) : std::string{"{}"};
            PluginxxScopeRef  scopeCopy{};
            const bool        hasScope = pluginxx_scope_ref_ok(scope) && scope;
            if (hasScope) {
                scopeCopy = *scope;
            }
            // 受理在调用线程完成 (句柄立即可用), 能力链在 IO 线程推进
            return mgr->invokeCapabilityAsync(
                inst,
                hasScope ? &scopeCopy : nullptr,
                id,
                args,
                phase,
                timeout_ms,
                cb,
                ud,
                error_out
            );
        });
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL capabilityInfoEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* capability_id,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(capability_id)) {
            setTableOpError(error_out, "capability_info: missing id");
            return nullptr;
        }
        const std::string id = svToStr(*capability_id);
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "capabilities.capability_info",
            [id](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                const std::string text = mgr->capabilityInfoText(id);
                if (text.empty()) {
                    throw TableOpError("capability_info: not_declared");
                }
                return text;
            },
            /*allowClosing=*/true
        );
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL capabilityListEntry(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    cb_ud,
        PluginxxString*          error_out
    ) {
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "capabilities.list_capabilities",
            [](InstanceT* inst, ManagerT* mgr) -> std::string {
                (void)inst;
                return mgr->listCapabilitiesText();
            },
            /*allowClosing=*/true
        );
    }

    // =====================================================================
    // scheduler 表
    // =====================================================================

    static int32_t PLUGINXX_CALL isIoThreadEntry(const PluginxxHost* host) {
        // 语义: “当前线程是不是该插件的执行线程”, 也就是“在这里阻塞会不会卡住插件”。
        // 名字沿用 ABI 老名 (避免换代), 但配了插件线程后回答的是插件线程。
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        return (mgr && mgr->isPluginThread()) ? 1 : 0;
    }

    static PluginxxOperatorHandle* PLUGINXX_CALL sleepEntry(
        const PluginxxHost*      host,
        int64_t                       ms,
        PluginxxOperatorCallback cb,
        void*                         ud,
        PluginxxString*          error_out
    ) {
        return guardVtableCall<PluginxxOperatorHandle*>(nullptr, [&]() {
            // 关闭中仍受理: stop 事务里的清理协程常用“入口立刻返回、后台继续收尾”,
            // 那条协程要能 sleep。租约按 lifecycle 放宽到 Closing, Closed 仍拒绝。
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !cb) {
                detail::setErrorString(error_out, "scheduler sleep: plugin runtime unavailable");
                return static_cast<PluginxxOperatorHandle*>(nullptr);
            }
            // sleep 自身会在调用线程受理、在 IO 线程建定时器; 这里不再阻塞等 IO
            return mgr->sleep(inst, ms, cb, ud, error_out);
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
            // 关闭中仍受理: 关闭期的清理常常要等一次阻塞工作 (join 自有线程前先收尾)。
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !work) {
                detail::setErrorString(error_out, "scheduler offload: plugin runtime unavailable");
                return static_cast<PluginxxOperatorHandle*>(nullptr);
            }
            // offload 在调用线程受理, 工作亲在 IO 线程交给工作池; 这里不再阻塞等 IO
            return mgr->offload(inst, work, done, ud, error_out);
        });
    }

    /// Operation 取消由 `<应用名>_plugin_cancel` 表唯一提供 (`op_cancel`), 本集合不再自带一份

    // =====================================================================
    // 协程驱动 (v2 起并入 scheduler 表)
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

    /// Operation 取消 (`<应用名>_plugin_cancel` 表的唯一入口; tasks 表复用它)
    static void PLUGINXX_CALL opCancelEntry(PluginxxOperatorHandle* op) {
        cancelPluginOperation(op);
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
            // 关闭中仍受理: `finishLater(notify)` + `ctx.spawn(...)` 是 stop/destroy
            // 入口的标准写法 (入口立刻返回, 清理协程继续跑)。这类任务的 lease 计入
            // inflight, 卸载的“等 inflight 归零”因此会等它跑完; Closed 仍拒绝。
            auto call = enterHost(host, /*allowClosing=*/true);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !notify) {
                return static_cast<PluginxxOperatorHandle*>(nullptr);
            }
            // 受理全在调用线程完成 (建 Operation + 填 notify), 不碰 IO 线程状态
            return mgr->registerTask(inst, cancel_fn, cancel_ud, notify, error_out);
        });
    }

    // =====================================================================
    // cancel 表
    // =====================================================================

    // =====================================================================
    // kind 表 (插件形态登记; 预置形态由主程序构造时交给注册表)
    // =====================================================================

    /// 登记 / 覆盖一种形态
    ///
    /// `spec_json` 字段见 `pluginxx/api/kinds.h`; `provider` 一律用登记它的实例名,
    /// 不接受自报 —— 卸载 / 停用时按实例名整批撤销。
    static PluginxxOperatorHandle* PLUGINXX_CALL kindRegisterEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* spec_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(spec_json)) {
            setTableOpError(error_out, "register_kind: missing spec_json");
            return nullptr;
        }
        const std::string raw = svToStr(*spec_json);
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "kind.register_kind",
            [raw](InstanceT* inst, ManagerT* mgr) -> std::string {
                utilxx_base::Json json;
                try {
                    json = utilxx_base::Json::parse(raw);
                } catch (const std::exception& e) {
                    throw TableOpError(
                        fmt::format("register_kind: invalid spec_json: {}", e.what())
                    );
                }
                if (!json.is_object()) {
                    throw TableOpError("register_kind: spec_json must be a JSON object");
                }
                PluginxxKindSpec spec;
                spec.kind                = json.value("kind", std::string{});
                spec.requiresPluginAbi   = json.value("requiresPluginAbi", false);
                spec.interfaceVersion    = json.value("interfaceVersion", 0);
                spec.minInterfaceVersion = json.value("minInterfaceVersion", 0);
                spec.loadCapability      = json.value("loadCapability", std::string{});
                spec.unloadCapability    = json.value("unloadCapability", std::string{});
                spec.validateCapability  = json.value("validateCapability", std::string{});
                spec.describe            = json.value("describe", std::string{});
                spec.provider            = inst->name;
                const bool overrideBuiltin = json.value("overrideBuiltin", false);
                std::string reason;
                if (!mgr->kindRegistry().registerKind(spec, overrideBuiltin, reason)) {
                    throw TableOpError(
                        reason.empty() ? std::string{"register_kind failed"} : std::move(reason)
                    );
                }
                return std::string{};
            }
        );
    }

    /// 撤销本插件登记的形态
    static PluginxxOperatorHandle* PLUGINXX_CALL kindUnregisterEntry(
        const PluginxxHost*       host,
        const PluginxxStringView* kind,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    ) {
        if (detail::abiViewEmpty(kind)) {
            setTableOpError(error_out, "unregister_kind: missing kind");
            return nullptr;
        }
        const std::string name = svToStr(*kind);
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "kind.unregister_kind",
            [name](InstanceT* inst, ManagerT* mgr) -> std::string {
                std::string reason;
                if (!mgr->kindRegistry().unregisterKind(name, inst->name, reason)) {
                    throw TableOpError(
                        reason.empty() ? std::string{"unregister_kind failed"} : std::move(reason)
                    );
                }
                return std::string{};
            }
        );
    }

    /// 列出全部形态 (预置的在前; 被接管的显示接管后的那一项)
    static PluginxxOperatorHandle* PLUGINXX_CALL kindListEntry(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    cb_ud,
        PluginxxString*          error_out
    ) {
        return detail::asyncTableOp<InstanceT, ManagerT>(
            host,
            cb,
            cb_ud,
            error_out,
            "kind.list_kinds",
            [](InstanceT*, ManagerT* mgr) -> std::string {
                utilxx_base::Json arr = utilxx_base::Json::array();
                for (const auto& spec : mgr->kindRegistry().all()) {
                    utilxx_base::Json item = utilxx_base::Json::object();
                    item["kind"]                = spec.kind;
                    item["builtin"]             = spec.builtin;
                    item["requiresPluginAbi"]   = spec.requiresPluginAbi;
                    item["interfaceVersion"]    = spec.interfaceVersion;
                    item["minInterfaceVersion"] = spec.minInterfaceVersion;
                    item["provider"]            = spec.provider;
                    item["providerId"]          = spec.provider;
                    item["loadCapability"]      = spec.loadCapability;
                    item["unloadCapability"]    = spec.unloadCapability;
                    item["validateCapability"]  = spec.validateCapability;
                    item["describe"]            = spec.describe;
                    item["overridden"]          = spec.builtin && !spec.provider.empty();
                    item["available"]           = !spec.loadCapability.empty();
                    arr.push_back(std::move(item));
                }
                return arr.dump();
            }
        );
    }


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
            &broadcastEndEntry,
            &broadcastSubscribeExEntry,
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
            &capabilityUnregisterExEntry,
        };
        return iface;
    }

    static const PluginxxSchedulerIface& schedulerIface() {
        static const PluginxxSchedulerIface iface{
            PLUGINXX_IFACE_SCHEDULER_VERSION,
            sizeof(PluginxxSchedulerIface),
            &isIoThreadEntry,
            &sleepEntry,
            &offloadEntry,
            &requestDriverEntry,
            &cancelDriverEntry,
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

    static const PluginxxKindIface& kindIface() {
        static const PluginxxKindIface iface{
            PLUGINXX_IFACE_KIND_VERSION,
            sizeof(PluginxxKindIface),
            &kindRegisterEntry,
            &kindUnregisterEntry,
            &kindListEntry,
        };
        return iface;
    }
};

/// 通用表的表名清单 (IID 里 `<应用名>_` 之后那一段, 均以 `plugin_` 开头):
/// 应用名不一致时用来给提示
inline constexpr std::string_view kGenericTableNames[] = {
    "plugin_log",   "plugin_json",        "plugin_config", "plugin_plugins",
    "plugin_broadcasts", "plugin_capabilities", "plugin_scheduler", "plugin_tasks",
    "plugin_cancel", "plugin_net", "plugin_kind",
};

/// `iid` 是否以 `_<已知通用表名>` 结尾
///
/// 只做后缀比较, 不按分切 (应用名 / 表名都可以含任意字符)。
inline bool looksLikeGenericTableIid(std::string_view iid) {
    for (const auto name : kGenericTableNames) {
        if (iid.size() <= name.size() + 1) {
            continue;
        }
        const auto suffixAt = iid.size() - name.size();
        if (iid[suffixAt - 1] == '_' && iid.compare(suffixAt, name.size(), name) == 0) {
            return true;
        }
    }
    return false;
}

/// 按 IID 查询**通用表** (未命中返回 nullptr, 主程序继续查自己的业务表)
/// - 覆盖 10 张通用表; IID 为编译期常量字符串, 比较用 string_view (不构造临时串)
/// - 未命中但形如 `<别的应用名>_<通用表名>` 时记一条日志: 主程序与插件必须用同一个
///   应用名 (`PLUGINXX_APP_NAME`) 编译, 否则插件拿不到表 (只提示, 不改行为);
///   本进程前缀的 IID 不属于这种情形 —— 它可能是主程序自己实现、名字与通用表同形的
///   表 (如网络表由主程序实现), 未命中就交给主程序继续查, 不唠叨
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
    if (iid == PLUGINXX_IFACE_TASKS) {
        return &Entries::tasksIface();
    }
    if (iid == PLUGINXX_IFACE_CANCEL) {
        return &Entries::cancelIface();
    }
    if (iid == PLUGINXX_IFACE_KIND) {
        return &Entries::kindIface();
    }
    if (!iid.starts_with(pluginxx::appTableIdPrefix()) && looksLikeGenericTableIid(iid)) {
        utilxx_base::xxLogPrint(
            utilxx_base::LogLevel::Warn,
            "pluginxx",
            "query_interface: `" + std::string{iid} + "` 未命中; 本进程的通用表 IID 以 `"
                + std::string{pluginxx::appTableIdPrefix()}
                + "` 开头 —— 主程序与插件必须用同一个应用名 (PLUGINXX_APP_NAME) 编译"
        );
    }
    return nullptr;
}

} // namespace pluginxx
