/// 插件开发 SDK —— 通用部分 (C++ header-only, 与主程序业务无关)
///
/// 归属: cxx_pluginxx (插件框架)。本头只提供**业务无关**的插件侧能力:
/// - [PluginStringView] / [PluginString]: 跨边界 ABI 字符串便捷工具与主程序堆字符串 RAII
/// - [queryInterface] / [PluginIfaceCore]: 通用接口表 (log / json / config / plugins /
///   broadcasts / capabilities / scheduler / coroutine_runtime / tasks / cancel) 查询
/// - [Logger] / [pluginLog] / [pluginStrdup] / [ctxGuardLogger] / [jsonEscape]: 实例级
///   日志与 JSON 工具
/// - [CancelRegistry] / [OpCtl]: 框架级取消登记与操作控制对象
/// - 插件协程一律是 `asio::awaitable`; 驱动桥 [detail::PollOneBridge] 按主机请求推进;
///   后台协作任务 [spawn]; 能力注册 [capabilityAsync]
/// - [ArgReader]: 强类型参数提取器
/// - [PluginBaseT]: 插件实例上下文基类 (业务表由主程序侧派生基类补齐)
/// - 导出宏 PLUGINXX_EXPORT_PLUGIN / PLUGINXX_EXPORT_PLUGIN_LIFECYCLE
///   (入口符号前缀由主程序给出; 主程序侧再包一层自己的导出宏)
///
/// 主程序业务部分 (工具 / 权限 / 回调 / 图节点 / client UI 等) 由主程序自己补: 主程序在
/// 自己的头文件里包含本头, 并用 `using` 把通用名引入本程序命名空间 —— 插件源码只包含
/// 主程序那份头即可, 无需关心本节分层。
#pragma once
#include "asio/async_result.hpp"
#include "asio/awaitable.hpp"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/cancellation_signal.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "pluginxx/api/abi.h"
#include "pluginxx/api/iface_meta.h"
#include "pluginxx/api/tables.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/json.h"
#include "utilxx_base/json_view.h"
#include <type_traits>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

/// asio 命名空间别名 (header-only SDK 不额外引入 asio_error.h, 避免插件多带依赖)
namespace asio = ::boost::asio;

namespace pluginxx {

/// 插件作用域 JSON 别名 (自主 Json 体系, 不依赖图引擎)
using Json     = utilxx_base::Json;
using JsonView = utilxx_base::JsonView;

/* ==================== C++ 字符串/接口便捷工具 (非 ABI) ====================
 *
 * `pluginxx/api/abi.h` 为纯 C ABI
 * (跨边界契约), 其结构体在 C++ 下仅带最小便捷成员
 * (构造/empty, 不改变布局)。面向主程序/插件 C++ 源码的字符串与接口操作集中
 * 在本命名空间:
 * - PluginStringView: 字符串视图便捷工具 (纯静态函数集合, 不持有状态;
 *   操作/返回跨边界 ABI 类型 PluginxxStringView/PluginxxString)
 * - PluginString: 主程序堆字符串 RAII (接管 PluginxxString 生命周期;
 *   析构自动经 host->vtable->free 释放)
 * - queryInterface<Iface>: 接口表查询模板
 *
 * 说明: 这些能力放在 C++ 头而非纯 C ABI 头 plugin_api.h, 因为按值返回含
 * C++ 成员函数的 struct 会触发 MSVC C4190。
 */

/// 字符串视图便捷工具 (纯静态函数; 不构造对象)
struct PluginStringView {
    /// 从 (指针, 长度) 构造 ABI 视图
    static PluginxxStringView from(const char* s, uint64_t n) noexcept {
        return PluginxxStringView{s, n};
    }

    /// 从 NUL 结尾 C 串构造 ABI 视图 (自动 strlen)
    static PluginxxStringView fromCstr(const char* s) noexcept {
        return PluginxxStringView{s, s ? static_cast<uint64_t>(std::strlen(s)) : 0};
    }

    /// 从 std::string_view 构造 ABI 视图
    static PluginxxStringView from(std::string_view s) noexcept {
        return PluginxxStringView{s.data(), static_cast<uint64_t>(s.size())};
    }

    /// ABI 视图是否为空
    static bool empty(const PluginxxStringView& sv) noexcept {
        return sv.data == nullptr || sv.size == 0;
    }

    /// 指针重载 (NULL 视为空)
    static bool empty(const PluginxxStringView* sv) noexcept {
        return sv == nullptr || sv->data == nullptr || sv->size == 0;
    }

    /// 主程序堆字符串是否为空
    static bool empty(const PluginxxString& s) noexcept {
        return s.data == nullptr || s.size == 0;
    }

    /// 指针重载 (NULL 视为空)
    static bool empty(const PluginxxString* s) noexcept {
        return s == nullptr || s->data == nullptr || s->size == 0;
    }

    /// ABI 视图 → std::string_view (零拷贝; NULL data 视为空串)
    static std::string_view str(const PluginxxStringView& sv) noexcept {
        return sv.data ? std::string_view{sv.data, static_cast<size_t>(sv.size)}
                       : std::string_view{};
    }

    /// 指针重载
    static std::string_view str(const PluginxxStringView* sv) noexcept {
        return (sv && sv->data) ? std::string_view{sv->data, static_cast<size_t>(sv->size)}
                                : std::string_view{};
    }

    /// 主程序堆字符串 → std::string_view (零拷贝; NULL data 视为空串)
    static std::string_view str(const PluginxxString& s) noexcept {
        return s.data ? std::string_view{s.data, static_cast<size_t>(s.size)} : std::string_view{};
    }

    /// 指针重载
    static std::string_view str(const PluginxxString* s) noexcept {
        return (s && s->data) ? std::string_view{s->data, static_cast<size_t>(s->size)}
                              : std::string_view{};
    }

    /// 主程序堆字符串 → ABI 视图
    static PluginxxStringView toSv(const PluginxxString& s) noexcept {
        return PluginxxStringView{s.data, s.size};
    }

    /// 指针重载 (NULL 视为空视图)
    static PluginxxStringView toSv(const PluginxxString* s) noexcept {
        return s ? PluginxxStringView{s->data, s->size} : PluginxxStringView{};
    }
};

/// 主程序堆字符串 RAII (析构自动释放; move-only)
class PluginString {
    const PluginxxHost* host_ = nullptr;
    PluginxxString      str_{nullptr, 0};

public:

    PluginString() = default;

    PluginString(const PluginxxHost* h, PluginxxString s) noexcept :
        host_(h),
        str_(s) {}

    ~PluginString() {
        reset();
    }

    PluginString(const PluginString&)            = delete;
    PluginString& operator=(const PluginString&) = delete;

    PluginString(PluginString&& o) noexcept :
        host_(o.host_),
        str_(o.str_) {
        o.host_ = nullptr;
        o.str_  = {nullptr, 0};
    }

    PluginString& operator=(PluginString&& o) noexcept {
        if (this != &o) {
            reset();
            host_   = o.host_;
            str_    = o.str_;
            o.host_ = nullptr;
            o.str_  = {nullptr, 0};
        }
        return *this;
    }

    /// 接管主程序出参 (fn 以 PluginxxString* 出参填充后接管所有权)
    template<typename Fn>
    static PluginString acquire(const PluginxxHost* h, Fn&& fn) {
        PluginxxString s{nullptr, 0};
        fn(&s);
        return PluginString(h, s);
    }

    /// 经主程序 alloc 拷贝视图 → ABI 主程序串;
    /// 返回裸 ABI 串, 调用方负责释放 (PluginString::free / 移入 PluginString RAII))
    static PluginxxString from(const PluginxxHost* h, const PluginxxStringView* sv) {
        PluginxxString res{nullptr, 0};
        if (!h || !h->vtable || !h->vtable->alloc || !sv || (!sv->data && sv->size == 0)) {
            return res;
        }
        char* p = static_cast<char*>(h->vtable->alloc(sv->size + 1));
        if (p) {
            if (sv->size > 0 && sv->data) {
                std::memcpy(p, sv->data, static_cast<size_t>(sv->size));
            }
            p[sv->size] = '\0';
            res.data    = p;
            res.size    = sv->size;
        }
        return res;
    }

    /// 引用重载
    static PluginxxString from(const PluginxxHost* h, const PluginxxStringView& sv) {
        return from(h, &sv);
    }

    /// std::string_view 重载
    static PluginxxString from(const PluginxxHost* h, std::string_view sv) {
        auto svAbi = PluginStringView::from(sv.data(), sv.size());
        return from(h, &svAbi);
    }

    /// 覆盖写入 ABI 字符串出参: 先释放已有主程序分配, 再经主程序 alloc 写入新内容
    static void set(const PluginxxHost* h, PluginxxString* out, std::string_view sv) noexcept {
        if (!out) {
            return;
        }
        if (out->data) {
            free(h, out);
        }
        *out = from(h, sv);
    }

    /// 从 std::string_view 经主程序 alloc 构造 RAII 对象
    static PluginString create(const PluginxxHost* h, std::string_view sv) {
        return PluginString(h, from(h, sv));
    }

    /// 从 ABI 视图经主程序 alloc 构造 RAII 对象
    static PluginString create(const PluginxxHost* h, const PluginxxStringView& sv) {
        return PluginString(h, from(h, &sv));
    }

    /// 经主程序 alloc 拷贝 C 串 → ABI 主程序串
    static PluginxxString fromCstr(const PluginxxHost* h, const char* s) {
        if (!h || !s) {
            return PluginxxString{nullptr, 0};
        }
        return from(h, PluginStringView::fromCstr(s));
    }

    /// 从 C 串经主程序 alloc 构造 RAII 对象
    static PluginString createCstr(const PluginxxHost* h, const char* s) {
        return PluginString(h, fromCstr(h, s));
    }

    /// 经主程序 alloc 拷贝视图为裸 char* (调用方负责 free)
    static char* strdup(const PluginxxHost* h, const PluginxxStringView* sv) {
        if (!h || !h->vtable || !h->vtable->alloc || !sv || (!sv->data && sv->size == 0)) {
            return nullptr;
        }
        char* p = static_cast<char*>(h->vtable->alloc(sv->size + 1));
        if (p) {
            if (sv->size > 0 && sv->data) {
                std::memcpy(p, sv->data, static_cast<size_t>(sv->size));
            }
            p[sv->size] = '\0';
        }
        return p;
    }

    /// 引用重载
    static char* strdup(const PluginxxHost* h, const PluginxxStringView& sv) {
        return strdup(h, &sv);
    }

    /// std::string_view 重载
    static char* strdup(const PluginxxHost* h, std::string_view sv) {
        auto svAbi = PluginStringView::from(sv.data(), sv.size());
        return strdup(h, &svAbi);
    }

    /// C 串重载
    static char* strdup(const PluginxxHost* h, const char* s) {
        if (!h || !s) {
            return nullptr;
        }
        return strdup(h, PluginStringView::fromCstr(s));
    }

    /// 释放主程序堆串 (幂等并清空)
    static void free(const PluginxxHost* h, PluginxxString* s) noexcept {
        if (s && s->data) {
            if (h && h->vtable && h->vtable->free) {
                h->vtable->free(s->data);
            }
            s->data = nullptr;
            s->size = 0;
        }
    }

    /// 释放本对象持有的串并复位
    void reset() noexcept {
        if (host_ && str_.data) {
            PluginString::free(host_, &str_);
        }
        host_ = nullptr;
        str_  = {nullptr, 0};
    }

    const char* c_str() const noexcept {
        return str_.data ? str_.data : "";
    }

    const char* data() const noexcept {
        return str_.data;
    }

    size_t size() const noexcept {
        return static_cast<size_t>(str_.size);
    }

    bool empty() const noexcept {
        return PluginStringView::empty(str_);
    }

    std::string_view view() const noexcept {
        return PluginStringView::str(PluginStringView::toSv(str_));
    }

    std::string str() const {
        return std::string(view());
    }

    PluginxxStringView to_sv() const noexcept {
        return PluginStringView::toSv(str_);
    }

    PluginxxString release() noexcept {
        PluginxxString tmp = str_;
        str_               = {nullptr, 0};
        host_              = nullptr;
        return tmp;
    }

    const PluginxxString& raw() const noexcept {
        return str_;
    }
};

/// 查询主程序接口表并转型 (IID → 接口表)
///
/// 判定规则 (每张表用自己的常量, 不再与字面量比较):
/// - `version >= IfaceMeta<Iface>::min_version` (语义级别兼容);
/// - `struct_size >= IfaceMeta<Iface>::baseline_size` (基线字节数够)。
///
/// 注意: **表可用不等于全部成员可用**。基线以内的成员由上面两条门槛保证; 表尾追加的
/// 成员必须用 `pluginxx::ifaceHas<&表::成员>(t)` 判定 (只判空指针会读到相邻内存,
/// 见 `pluginxx/api/iface_meta.h`)。
template<typename Iface>
const Iface* validateInterface(const void* raw) noexcept {
    if (!raw) {
        return nullptr;
    }
    const auto* iface = static_cast<const Iface*>(raw);
    if (iface->version < IfaceMeta<Iface>::min_version
        || iface->struct_size < IfaceMeta<Iface>::baseline_size) {
        return nullptr;
    }
    return iface;
}

template<typename Iface>
const Iface* queryInterface(const PluginxxHost* host, std::string_view iid) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface || iid.empty()) {
        return nullptr;
    }
    PluginxxStringView sv = PluginStringView::from(iid.data(), iid.size());
    return validateInterface<Iface>(host->vtable->query_interface(host, &sv));
}

template<typename Iface>
const Iface* queryInterface(const PluginxxHost* host, const PluginxxStringView& iid) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface || PluginStringView::empty(iid)) {
        return nullptr;
    }
    return validateInterface<Iface>(host->vtable->query_interface(host, &iid));
}

template<typename Iface>
const Iface* queryInterface(const PluginxxHost* host, const char* iid) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface || !iid) {
        return nullptr;
    }
    PluginxxStringView sv = PluginStringView::fromCstr(iid);
    return validateInterface<Iface>(host->vtable->query_interface(host, &sv));
}

/* ==================== 通用接口表聚合 ==================== */

/// 通用接口表聚合 (与主程序业务无关的十个表; 成员为 NULL 表示主程序未实现该接口)
///
/// 业务表 (工具 / 权限 / 回调 / 会话 / 模型 / 提示词 / 资源 / 图, 以及 client 侧 UI)
/// 由主程序在自己的聚合体中补充: 主程序那侧的聚合体是本聚合的超集,
/// 可直接作为 [PluginBaseT] 的模板实参。
struct PluginIfaceCore {
    const PluginxxLogIface*          log          = nullptr; ///< "<应用名>_plugin_log"
    const PluginxxJsonIface*         json         = nullptr; ///< "<应用名>_plugin_json"
    const PluginxxConfigIface*       config       = nullptr; ///< "<应用名>_plugin_config"
    const PluginxxPluginsIface*      plugins      = nullptr; ///< "<应用名>_plugin_plugins"
    const PluginxxBroadcastsIface*   broadcasts   = nullptr; ///< "<应用名>_plugin_broadcasts"
    const PluginxxCapabilitiesIface* capabilities = nullptr; ///< "<应用名>_plugin_capabilities"
    const PluginxxSchedulerIface*    scheduler    = nullptr; ///< "<应用名>_plugin_scheduler"
    /// "<应用名>_plugin_cancel": 异步操作句柄的唯一取消入口
    const PluginxxCancelIface* cancel = nullptr;
    const PluginxxTasksIface*  tasks  = nullptr; ///< "<应用名>_plugin_tasks"

    /// 从主程序查询全部通用接口表 (host 为空时返回全 NULL 聚合)
    static PluginIfaceCore query(const PluginxxHost* host) {
        PluginIfaceCore f;
        if (!host || !host->vtable || !host->vtable->query_interface) {
            return f;
        }
        f.log     = queryInterface<PluginxxLogIface>(host, PLUGINXX_IFACE_LOG);
        f.json    = queryInterface<PluginxxJsonIface>(host, PLUGINXX_IFACE_JSON);
        f.config  = queryInterface<PluginxxConfigIface>(host, PLUGINXX_IFACE_CONFIG);
        f.plugins = queryInterface<PluginxxPluginsIface>(host, PLUGINXX_IFACE_PLUGINS);
        f.broadcasts
            = queryInterface<PluginxxBroadcastsIface>(host, PLUGINXX_IFACE_BROADCASTS);
        f.capabilities
            = queryInterface<PluginxxCapabilitiesIface>(host, PLUGINXX_IFACE_CAPABILITIES);
        f.scheduler = queryInterface<PluginxxSchedulerIface>(host, PLUGINXX_IFACE_SCHEDULER);
        f.tasks     = queryInterface<PluginxxTasksIface>(host, PLUGINXX_IFACE_TASKS);
        f.cancel    = queryInterface<PluginxxCancelIface>(host, PLUGINXX_IFACE_CANCEL);
        return f;
    }
};

/* ==================== 取消异常 ==================== */

class CanceledException : public std::exception {
    std::string msg_;

public:

    explicit CanceledException(std::string msg = "operation canceled") :
        msg_(std::move(msg)) {}

    const char* what() const noexcept override {
        return msg_.c_str();
    }
};

/* ==================== 实例级 Logger ==================== */

struct Logger {
    const PluginxxHost*     host     = nullptr;
    const PluginxxLogIface* logIface = nullptr;
    void(PLUGINXX_CALL*
             logFn)(const PluginxxHost* host, int32_t level, const PluginxxStringView* msg)
        = nullptr;

    void log(int32_t level, std::string_view msg) const noexcept {
        if (!host) {
            return;
        }
        if (logFn) {
            auto sv = PluginStringView::from(msg.data(), msg.size());
            logFn(host, level, &sv);
        } else if (logIface && logIface->log) {
            auto sv = PluginStringView::from(msg.data(), msg.size());
            logIface->log(host, level, &sv);
        }
    }

    void trace(std::string_view msg) const noexcept {
        log(0, msg);
    }

    void debug(std::string_view msg) const noexcept {
        log(1, msg);
    }

    void info(std::string_view msg) const noexcept {
        log(2, msg);
    }

    void warn(std::string_view msg) const noexcept {
        log(3, msg);
    }

    void error(std::string_view msg) const noexcept {
        log(4, msg);
    }
};

/* ==================== 公共便捷助手 ====================
 *
 * 插件各处反复手写的三件小事, 统一由 SDK 提供, 插件内可
 * `using pluginxx::xxx;` 引入后按原名调用:
 * - [pluginLog]        经实例日志接口输出 (上下文为空时静默)
 * - [pluginStrdup]     经主程序 alloc 复制 C 串 (C ABI 出参直接赋值用)
 * - [ctxGuardLogger]   C ABI 边界异常守卫 (`guardCall`/`guardCallVoid`) 的日志闭包
 */

/// 实例日志便捷函数 (ctx 为空时静默)
///
/// - `args`:
///     - [ctx] 插件实例上下文 (须继承 [PluginBaseT], 即具备 `log` 成员)
///     - [level] 日志等级: 0=trace 1=debug 2=info 3=warn 4=error
///     - [msg] 日志内容
template<typename Ctx>
inline void pluginLog(const Ctx* ctx, int32_t level, std::string_view msg) {
    if (ctx) {
        ctx->log.log(level, msg);
    }
}

/// 实例日志便捷函数 (直接持有主程序与日志接口表、无实例上下文的插件使用)
///
/// - 已格式化的内容原样输出 (不做前缀/截断; 异常上报路径见 [logTo])
/// - 主程序或日志接口缺失时静默丢弃
///
/// - `args`:
///     - [host] 主程序句柄 (由插件入口传入, 须非空)
///     - [logIf] 日志接口表 (经主程序 query_interface 取得)
///     - [level] 日志等级: 0=trace 1=debug 2=info 3=warn 4=error
///     - [msg] 日志内容
inline void pluginLog(
    const PluginxxHost*     host,
    const PluginxxLogIface* logIf,
    int32_t                 level,
    std::string_view        msg
) {
    if (!host || !logIf || !logIf->log) {
        return;
    }
    auto sv = PluginStringView::from(msg.data(), msg.size());
    logIf->log(host, level, &sv);
}

/// 经主程序 alloc 复制 C 串, 供 C ABI 出参 (`char*`) 直接赋值
/// - 包装 [PluginString::strdup], 省去每处手写视图构造
/// - 与 `pluginStrdup` 语义一致: 主程序或入参为空返回 nullptr
/// - `return` 主程序堆内存, 调用方负责经主程序 free 释放
inline char* pluginStrdup(const PluginxxHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    auto sv = PluginStringView::fromCstr(s);
    return PluginString::strdup(host, &sv);
}

/// 生成 C ABI 边界异常守卫使用的日志闭包 (error 级; ctx 为空时静默)
///
/// - 用法: `pluginxx::guardCallVoid(ctxGuardLogger(ctx), [&] { ... });`
/// - `return` 可拷贝的日志闭包 (仅捕获上下文指针, 不持有实例所有权)
template<typename Ctx>
inline auto ctxGuardLogger(Ctx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

/// 文本 → JSON 字符串字面量 (含首尾双引号), 供手工拼装 JSON 文本的插件使用
///
/// - 优先经主程序 `json_escape` 接口转义 (正确处理引号/反斜杠/控制字符/非 ASCII);
///   接口缺失或调用失败时回退为本地转义 (转义 `"` `\` 与 ASCII 控制字符, 其余原样),
///   保证结果始终是合法 JSON 字符串字面量
/// - 常用场景: 把插件侧构造的字符串 (插件名/脚本路径/MCP 地址等) 拼进 JSON 文本
///   (如 `fmt::format("{{\"name\":{}}}", jsonEscape(...))`)
///
/// - `args`:
///     - [host] 主程序句柄
///     - [jsonIface] 主程序 json 接口表 (主程序多种插件共用, 可空)
///     - [text] 待转义文本 (内容可含任意字节, 含非法 UTF-8 时应先自行修复)
///
/// - `return` 形如 `"..."` 的 JSON 字符串字面量
template<typename JsonIface>
inline std::string
    jsonEscape(const PluginxxHost* host, const JsonIface* jsonIface, std::string_view text) {
    if (host && jsonIface && jsonIface->json_escape) {
        PluginxxString esc{nullptr, 0};
        auto           sv = PluginStringView::from(text.data(), text.size());
        if (jsonIface->json_escape(host, &sv, &esc) == 0 && esc.data) {
            std::string out(esc.data, static_cast<size_t>(esc.size));
            PluginString::free(host, &esc);
            return out;
        }
    }
    // 回退: 最小化转义 (主程序接口不可用时仍给出合法 JSON 字符串)
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (uc < 0x20) {
                    constexpr char kHex[]  = "0123456789abcdef";
                    out                   += "\\u00";
                    out.push_back(kHex[(uc >> 4) & 0xfu]);
                    out.push_back(kHex[uc & 0xfu]);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

/* ==================== 协程驱动桥 (PollOneBridge) ====================
 *
 * 定位: 让插件协程与主程序协程在**同一主程序 IO 执行序列**中交错推进的适配层。
 *
 * 协议 (与协程库无关):
 * - 插件侧适配器把本地协程的推进单位记为"一次有限步骤" (poll_one 一个就绪 handler);
 * - 只要本地有**已知**可运行工作 (新 root 首步 / 主程序回调完成投递的 continuation /
 *   本地 post), 就调用 [wake]; wake 合并重复请求后向主程序申请一次驱动请求;
 * - 主程序把请求异步投递到自己的 IO 线程, 回调里只做一次 `poll_one()`; 完成后若又
 *   有新 wake 才申请下一次请求; 没有新工作时**不再申请**, 因此空闲时不消耗主程序
 *   任务队列, 也不自旋;
 * - **绝不在主程序回调栈内直接恢复插件协程**: 主程序回调只复制结果并
 *   `postToLocal + wake`, continuation 仍由下一次 driver 推进。
 *
 * 状态机 (三个竞态窗口都必须覆盖: driver 前 / driver 执行中 / driver 返回后):
 * - `readySteps_`: 已投递但尚未执行的本地步骤数 (每一步需要一次 poll_one);
 * - `wakePending_`: 显式 wake 尚未被请求覆盖 (覆盖"插件直接向 local_executor 投递,
 *   kit 看不到该投递"的用法; 每次 wake 至多多花一次请求, 不会自旋);
 * - `driverQueued_`: 已申请请求 (含 request_driver 正在返回的窗口);
 * - `driverRunning_`: 回调正在执行;
 * 单个 `bool scheduled` 会在"回调收尾清标志"与"外部投递写标志"之间丢通知: 例如
 * 并发启动 N 个根时, N 次 wake 若被合并成一次请求, 就只有 1 个根会被推进。因此
 * 这里用"步骤计数 + 显式 wake 标记 + 两个进行中标志 + epoch"组合, 保证任何窗口中的
 * 投递最终都会被推进, 且没有新工作时请求数不再增长。
 *
 * 线程: `wake()` 可从外部完成回调线程调用; `driveOnce()` 在主程序 IO 线程执行;
 * `stop()` 从 stop/destroy 路径调用。三者用一把**短**临界区互斥线性化 —— 临界区内
 * 不调用插件业务代码、不投递、不等待, 因此既不会与主程序形成死锁, 也不影响
 * "插件状态只在主程序 IO 线程访问"这一无锁前提 (真正的插件代码只在 driveOnce 的
 * `poll_one` 里跑)。
 */
namespace detail {

/// 单实例协程驱动桥 (定义在下方)。
class PollOneBridge;

/// 受控轮询根 (asio 协程) 的仲裁对象。
///
/// `asio::awaitable` 的协程帧由 asio 自己的 completion handler 持有 (本地 reactor
/// 销毁时统一释放), 因此本对象**不负责销毁帧**, 只负责:
/// - 终态上报与资源释放的 **exactly-once** 仲裁 (正常完成 vs 桥停止时放弃);
/// - 被放弃时执行一次类型擦除的清理 (回收 Job 等主程序可见资源)。
///
/// (完成/放弃两条路径都可能释放同一个 Job, 因此用一次 CAS 决定谁来做。)
class PolledRoot : public std::enable_shared_from_this<PolledRoot> {
public:

    explicit PolledRoot(const PluginxxOperatorNotify& notify) :
        notify_(notify) {}

    PolledRoot(const PolledRoot&)            = delete;
    PolledRoot& operator=(const PolledRoot&) = delete;

    ~PolledRoot() {
        runCleanup();
    }

    /// 认领"根已终结"的所有权 (CAS); 只有第一个调用者能走完成/放弃流程。
    bool claimFinish() noexcept {
        bool expected = false;
        return claimed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    bool finished() const noexcept {
        return claimed_.load(std::memory_order_acquire);
    }

    /// 向主程序上报终态 (仅在 [claimFinish] 成功后调用, 保证 exactly-once)。
    void notifyHost(int32_t status, std::string_view payload) noexcept {
        if (!notify_.done) {
            return;
        }
        auto sv = PluginStringView::from(payload.data(), payload.size());
        try {
            notify_.done(notify_.host_ud, status, &sv);
        } catch (...) {
            // 主程序回调不得抛异常; 即便抛了也已经认领过完成权, 不重复派发。
        }
    }

    /// 设置资源清理回调 (回收 Job); 由 [claimFinish] 的赢家执行恰好一次。
    void setCleanup(std::function<void()> cleanup) {
        cleanup_ = std::move(cleanup);
    }

    /// 执行清理 (幂等)。先把回调移出再调用, 避免回调内部释放本对象导致
    /// "在成员函数内自销毁"。
    void runCleanup() noexcept {
        if (!cleanup_) {
            return;
        }
        auto cleanup = std::move(cleanup_);
        cleanup_     = nullptr;
        try {
            cleanup();
        } catch (...) {
        }
    }

private:

    PluginxxOperatorNotify notify_{nullptr, nullptr};
    std::function<void()>  cleanup_;
    std::atomic<bool>      claimed_{false};
};

/// 单实例协程驱动桥 (每个插件实例独立一份; 无任何进程级可变状态)。
///
/// kit 内部用法:
/// - 协程首步 / 主程序回调完成: `postToLocal(...)` + `wake()`;
/// - stop/destroy: [stop] (拒绝新 wake、取消排队请求、停止本地 reactor、终结未结束的
///   受控轮询根)。
///
/// 根只有一类: **受控轮询根** ([PolledRoot], 声明式受控轮询的工具 / 后台任务)。
/// 它等的是插件本地 reactor 上的内核就绪事件 (socket/管道/文件/本地 timer)。
/// `poll_one` 只能"执行已就绪 handler", 不会让等待对象到期, 因此这类根需要"有操作
/// 进行中时轮询": 有进展立即续票、无进展退避 `kPollIntervalMs`, 空闲 (无未结束的
/// polled 操作) 时不申请请求、不建定时器。
class PollOneBridge {
public:

    using LocalExecutor = asio::io_context::executor_type;

    /// 连续多少次 driver 无进展后提示"等待没有主程序可见的唤醒源"。
    /// 取 8 是为了容忍合理的偶发空转 (同一轮 wake 与已被处理的 continuation 重叠),
    /// 又不至于让真正的私有 reactor 等待被静默掩盖。
    static constexpr int kNoProgressWarnStreak = 8;

    /// ---- 受控轮询 (pump) 参数: 只作用于受控轮询根 (工具/能力/图节点/后台任务的协程体) ----
    /// 无进展时的退避时间片 (ms): 等待插件框架就绪期间约 100 次/秒驱动,
    /// 每次 ≈1 个主程序 post + 1 次 `epoll_wait(0)`, 空闲时不产生任何驱动。
    static constexpr int64_t kPollIntervalMs = 10;
    /// 连续"有进展"多少步后强制让出一次 (给主程序其它任务与同实例其它操作机会),
    /// 避免插件自循环 (自己给自己 post) 长期独占 IO 线程。
    static constexpr int kPollBurstMax = 256;
    /// 突发上限触发后的让出时长 (ms)。
    static constexpr int64_t kPollBurstYieldMs = 1;

    PollOneBridge(
        const PluginxxHost*           host,
        const PluginxxSchedulerIface* scheduler,
        const PluginxxCancelIface*    cancel = nullptr
    ) :
        localIo_(1),
        work_(asio::make_work_guard(localIo_)),
        host_(host),
        sched_(scheduler),
        cancel_(cancel) {}

    PollOneBridge(const PollOneBridge&)            = delete;
    PollOneBridge& operator=(const PollOneBridge&) = delete;

    ~PollOneBridge() {
        stop();
        // 被放弃的受控轮询根: 协程帧由 asio 随本地 reactor 释放 (见 [stop]),
        // 这里回收它们的 Job (清理回调幂等, 正常完成时已执行过)。
        std::vector<std::function<void()>> cleanups;
        {
            std::lock_guard lock(rootsMutex_);
            cleanups = std::move(abandonedCleanups_);
            abandonedCleanups_.clear();
        }
        cleanups.clear();
    }

    /// 本地执行器: 插件协程首步/continuation 的排队目标 (由 host driver 推进)。
    LocalExecutor local_executor() noexcept {
        return localIo_.get_executor();
    }

    const PluginxxHost* host() const noexcept {
        return host_;
    }

    const PluginxxSchedulerIface* scheduler() const noexcept {
        return sched_;
    }

    const PluginxxCancelIface* cancelIface() const noexcept {
        return cancel_;
    }

    /// 是否仍有已知可运行工作 (显式 wake 未被请求覆盖, 或有已投递未执行的步骤)。
    bool hasPendingWake() const noexcept {
        std::lock_guard lock(mutex_);
        return wakePending_ || readySteps_.load(std::memory_order_acquire) > 0;
    }

    /// 已投递但尚未执行的本地步骤数 (诊断)。
    uint64_t readySteps() const noexcept {
        return readySteps_.load(std::memory_order_acquire);
    }

    bool isDriverQueued() const noexcept {
        std::lock_guard lock(mutex_);
        return driverQueued_;
    }

    bool isDriverRunning() const noexcept {
        std::lock_guard lock(mutex_);
        return driverRunning_;
    }

    bool isStopping() const noexcept {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    /// 已申请的请求总数 (诊断: 用于验证"空闲时请求数不再增长")。
    uint64_t ticketsIssued() const noexcept {
        return ticketsIssued_.load(std::memory_order_acquire);
    }

    /// 已完成的驱动步数 (诊断)。
    uint64_t driverSteps() const noexcept {
        return driverSteps_.load(std::memory_order_acquire);
    }

    /// 未结束的受控轮询操作数 (诊断: 0 表示"不轮询、不建定时器")。
    uint64_t polledRootCount() const noexcept {
        return polledRoots_.load(std::memory_order_acquire);
    }

    /// 已发生的"无进展退避"次数 (诊断: 验证退避确实发生且空闲时不增长)。
    uint64_t idlePollCount() const noexcept {
        return idlePollCount_.load(std::memory_order_acquire);
    }

    /// 是否有已排定的退避定时器 (诊断)。
    bool isPumpWaitScheduled() const {
        std::lock_guard lock(mutex_);
        return pumpWaitScheduled_;
    }

    /// 是否正在受控轮询 (有未结束的 polled 操作)。
    bool isPumping() const noexcept {
        return polledRoots_.load(std::memory_order_acquire) > 0;
    }

    /// 当前线程是否主程序 IO 线程 (仅诊断; 不得据此内联执行 driver 回调)。
    bool onHostIoThread() const noexcept {
        if (!host_ || !sched_ || !sched_->is_io_thread) {
            return false;
        }
        return sched_->is_io_thread(host_) != 0;
    }

    /// 把一段插件侧代码投递到本地执行器, 由后续 host driver 推进。
    /// - 恒异步 (即使调用者就在本地执行上下文内): 这是"一轮只推进一个 continuation"
    ///   的实现基础;
    /// - 不自动 wake: 调用方投递后必须调用 [wake] (语义上"投递 = 有工作")。
    void postToLocal(std::function<void()> fn) {
        if (!fn) {
            return;
        }
        readySteps_.fetch_add(1, std::memory_order_acq_rel);
        auto* self = this;
        asio::post(localIo_, [self, fn = std::move(fn)]() mutable {
            // 本 handler 已被 host driver 选中执行: 处理投递时记下的那一步。
            self->readySteps_.fetch_sub(1, std::memory_order_acq_rel);
            try {
                fn();
            } catch (const std::exception& e) {
                logFallback(fmt::format("plugin local task threw: {}", e.what()));
            } catch (...) {
                logFallback("plugin local task threw unknown exception");
            }
        });
    }

    /// 登记一个受控轮询根 (工具/能力/图节点/后台任务的协程体)。
    ///
    /// 必须在把该协程排进本地执行器之前调用: 登记之后 `polledRoots_ > 0`,
    /// 桥才知道"等待期间需要按受控轮询策略继续申请请求"。
    void addPolledRoot(const std::shared_ptr<PolledRoot>& root) {
        if (!root) {
            return;
        }
        std::lock_guard lock(rootsMutex_);
        polledRootList_.push_back(root);
        polledRoots_.store(polledRootList_.size(), std::memory_order_release);
    }

    /// 注销一个受控轮询根 (协程结束后调用)。
    /// 计数归零时停止轮询: 取消已排定的退避定时器, 之后不再申请请求。
    void removePolledRoot(const std::shared_ptr<PolledRoot>& root) noexcept {
        bool                    last   = false;
        PluginxxOperatorHandle* waitOp = nullptr;
        {
            std::lock_guard lock(rootsMutex_);
            const auto      it = std::find(polledRootList_.begin(), polledRootList_.end(), root);
            if (it == polledRootList_.end()) {
                // 已被 [failAllPolledRoots] 清理 (停桥路径): 只保留终态仲裁语义,
                // 不再触碰 pump 状态 (Job 的回收由该路径负责)。
                return;
            }
            polledRootList_.erase(it);
            polledRoots_.store(polledRootList_.size(), std::memory_order_release);
            last = polledRootList_.empty();
            if (last) {
                std::lock_guard pumpLock(mutex_);
                waitOp = takePumpWaitLocked();
            }
        }
        if (last) {
            cancelSchedulerOp(waitOp);
        }
    }

    /// 立即结束已排定的退避 (取消退避定时器, 让下一次驱动马上到来)。
    /// 用于 `execute_cancel`: 插件不必等满一个退避时间片就能看到取消并收束根。
    void kickPumpWait() noexcept {
        PluginxxOperatorHandle* waitOp = nullptr;
        {
            std::lock_guard lock(mutex_);
            waitOp = takePumpWaitLocked();
        }
        cancelSchedulerOp(waitOp);
    }

    /// 主程序拒绝再提供驱动 / 桥停止时, 终结全部未结束的受控轮询根 (幂等)。
    ///
    /// 帧不在这里销毁 (由 asio 随本地 reactor 释放); 这里只负责:
    /// - 每个根按 FAILED 上报一次 (`claimFinish` 仲裁, 与正常完成路径互斥);
    /// - 执行类型擦除清理 (回收 Job), 避免放弃路径泄漏插件侧资源。
    void failAllPolledRoots(std::string_view reason) noexcept {
        std::vector<std::shared_ptr<PolledRoot>> roots;
        {
            std::lock_guard lock(rootsMutex_);
            roots = std::move(polledRootList_);
            polledRootList_.clear();
            polledRoots_.store(0, std::memory_order_release);
        }
        for (const auto& root : roots) {
            if (!root || !root->claimFinish()) {
                continue;
            }
            root->notifyHost(PLUGINXX_OPERATOR_FAILED, reason);
            // `roots` 在循环期间持有强引用: 清理回调释放 Job 时不会被自销毁打断。
            root->runCleanup();
        }
    }

    /// 本地已有可运行工作 (新 root 首步 / 主程序回调 continuation / 本地 post 之后)。
    ///
    /// 幂等且可合并: 同一实例同时最多登记一次请求; 没有新工作时**不会**继续申请,
    /// 因此空闲时不消耗主程序任务队列, 也不自旋。
    void wake() noexcept {
        bool needRequest = false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            wakePending_ = true;
            needRequest  = prepareScheduleLocked();
        }
        if (needRequest) {
            requestDriverNow();
        }
    }

    /// 停止接入 (stop/destroy 路径):
    /// - 拒绝新的 wake 与请求申请;
    /// - 取消**尚未开始**的请求 (正在执行的回调不打断, 由插件自己的 root 收束协议收尾);
    /// - 停止本地 reactor 并释放 work guard: 之后即便有迟到请求也不会再执行插件代码;
    /// - 活跃根统一终结为失败 (主程序不会再有驱动来推进它们); 本地排队的 continuation
    ///   随桥析构释放 (asio 协程帧由本地 reactor 释放)。
    void stop() noexcept {
        PluginxxDriver*         toCancel = nullptr;
        PluginxxOperatorHandle* waitOp   = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_    = true;
            wakePending_ = false;
            pumpPending_ = false;
            if (driverQueued_) {
                // driver_ 为空表示 request_driver 正在返回窗口: 该分支由
                // [requestDriverNow] 观察到停止状态后自行取消请求。
                toCancel      = driver_;
                driver_       = nullptr;
                driverQueued_ = false;
                pendingEpoch_ = 0;
            }
            waitOp = takePumpWaitLocked();
        }
        cancelTicket(toCancel);
        cancelSchedulerOp(waitOp);
        work_.reset();
        localIo_.stop();
        failAllPolledRoots("plugin coroutine bridge stopped");
    }

    void logWarn(std::string_view message) const noexcept {
        logToHost(host_, 3, message);
    }

    void logError(std::string_view message) const noexcept {
        logToHost(host_, 4, message);
    }

    /// 静态上下文 (asio handler 内部) 使用的回退日志入口。
    static void logFallback(std::string_view message) noexcept {
        std::fputs("[plugin kit] ", stderr);
        std::fwrite(message.data(), 1, message.size(), stderr);
        std::fputc('\n', stderr);
    }

    /// 经主程序日志接口表输出 (缺失时退回 stderr); 不保存跨实例状态。
    static void
        logToHost(const PluginxxHost* host, int32_t level, std::string_view message) noexcept {
        if (!host || !host->vtable || !host->vtable->query_interface) {
            logFallback(message);
            return;
        }
        PluginxxStringView iid = PluginStringView::fromCstr(PLUGINXX_IFACE_LOG);
        const auto*        logIface
            = static_cast<const PluginxxLogIface*>(host->vtable->query_interface(host, &iid));
        if (!logIface || !logIface->log) {
            logFallback(message);
            return;
        }
        try {
            PluginxxStringView msg = PluginStringView::from(message.data(), message.size());
            logIface->log(host, level, &msg);
        } catch (...) {
        }
    }

private:

    /// 申请请求前的状态迁移 (调用方须持有 [mutex_])。
    /// `return`: true = 调用方应立刻调用 [requestDriverNow]
    bool prepareScheduleLocked() noexcept {
        if (stopping_ || driverQueued_ || driverRunning_) {
            return false;
        }
        // 只在"确实还有可运行工作"时申请: 已投递未执行的步骤, 或一次尚未被覆盖的
        // 显式 wake (覆盖插件直接向 local_executor 投递、kit 看不到的用法), 或
        // 受控轮询判定"该继续驱动" (pumpPending_, 见 [pumpNextLocked])。
        if (readySteps_.load(std::memory_order_acquire) == 0 && !wakePending_ && !pumpPending_) {
            return false;
        }
        wakePending_  = false;
        pumpPending_  = false;
        driverQueued_ = true;
        pendingEpoch_ = ++nextEpoch_;
        ticketsIssued_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    /// 向主程序申请请求 (可在任意线程执行; 主程序保证异步投递回调, 且每张至多一次)。
    void requestDriverNow() noexcept {
        uint64_t epoch = 0;
        {
            std::lock_guard lock(mutex_);
            epoch = pendingEpoch_;
        }
        if (epoch == 0) {
            return;
        }
        PluginxxString  error{nullptr, 0};
        PluginxxDriver* ticket
            = sched_->request_driver(host_, &PollOneBridge::driveOnceTrampoline, this, &error);
        if (!ticket) {
            std::string message = "host refused coroutine driver";
            if (error.data) {
                message.assign(error.data, static_cast<size_t>(error.size));
                PluginString::free(host_, &error);
            }
            {
                std::lock_guard lock(mutex_);
                if (pendingEpoch_ == epoch) {
                    pendingEpoch_ = 0;
                    driverQueued_ = false;
                }
            }
            logError(fmt::format("coroutine driver request failed: {}", message));
            failAllPolledRoots(message);
            return;
        }
        bool cancelImmediately = false;
        {
            std::lock_guard lock(mutex_);
            // 该请求仍归本轮所有 (回调尚未处理它) 时才记录句柄; 否则说明回调已经
            // 跑完 (或实例已停止): 此时请求已收束, 取消是幂等空操作。
            if (pendingEpoch_ == epoch && driverQueued_ && !stopping_) {
                driver_ = ticket;
            } else {
                cancelImmediately = true;
            }
        }
        if (cancelImmediately) {
            cancelTicket(ticket);
        }
    }

    void cancelTicket(PluginxxDriver* ticket) noexcept {
        if (ticket && sched_ && sched_->cancel_driver) {
            sched_->cancel_driver(ticket);
        }
    }

    static void PLUGINXX_CALL driveOnceTrampoline(void* ud) {
        auto* self = static_cast<PollOneBridge*>(ud);
        if (!self) {
            return;
        }
        self->driveOnce();
    }

    /// host driver 回调 (主程序 IO 线程): 严格推进**一个**有限步骤。
    void driveOnce() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                // 停止后到达的迟到请求: 不执行任何插件代码 (请求由主程序自行收束)。
                driverQueued_ = false;
                driver_       = nullptr;
                pendingEpoch_ = 0;
                return;
            }
            driverQueued_  = false;
            driver_        = nullptr; // 请求已处理, 不再需要取消
            pendingEpoch_  = 0;
            driverRunning_ = true;
        }

        if (!ranOnIoThread_.exchange(true, std::memory_order_acq_rel) && !onHostIoThread()) {
            // 首次驱动做一次线程断言 (仅诊断; 不影响语义)。
            logWarn("coroutine driver is not running on the host IO thread; plugin state must not "
                    "be shared with other threads");
        }

        std::size_t progressed = 0;
        try {
            // 严格只调用一次: 一个 driver 对应一个局部 continuation, 主程序与插件任务
            // 因此自然轮换, 单个插件无法长时间独占主程序 executor。
            progressed = localIo_.poll_one();
        } catch (const std::exception& e) {
            logError(fmt::format("plugin local runtime step threw: {}", e.what()));
        } catch (...) {
            logError("plugin local runtime step threw unknown exception");
        }

        // 受控轮询期间"本轮无进展"是常态 (等待插件框架就绪 / 退避), 不算异常; 因此
        // 警告只在"没有 polled 操作在轮询、却持续空转"时出现 —— 那正是"插件依赖私有
        // reactor 却没有声明 polled"的误用信号。
        const bool pumping = polledRoots_.load(std::memory_order_acquire) > 0;
        if (progressed == 0 && !pumping) {
            if (++noProgressStreak_ >= kNoProgressWarnStreak) {
                noProgressStreak_ = 0;
                logWarn(
                    "coroutine driver made no progress: the awaited work has no host-visible "
                    "wake source (private reactor waits are not driven by this bridge); use host "
                    "callbacks, the host timer adapter, or declare the tool as polled"
                );
            }
        } else {
            noProgressStreak_ = 0;
        }
        driverSteps_.fetch_add(1, std::memory_order_acq_rel);

        bool    needRequest = false;
        bool    backoff     = false;
        int64_t backoffMs   = 0;
        {
            std::lock_guard lock(mutex_);
            driverRunning_ = false;
            // 只处理本轮之前或本轮中出现的真实 wake; 没有新 wake 就停止, 外部事件
            // 到达时会自己调用 [wake]。
            needRequest = prepareScheduleLocked();
            if (!needRequest) {
                // 受控轮询: 有未结束的 polled 操作时按"有进展立即续, 无进展退避"继续，
                // 否则不申请请求 (空闲零开销)。
                switch (pumpNextLocked(progressed, backoffMs)) {
                    case PumpDecision::Immediate:
                        needRequest = prepareScheduleLocked();
                        break;
                    case PumpDecision::Backoff:
                        backoff = true;
                        break;
                    case PumpDecision::Stop:
                        break;
                }
            }
        }
        if (needRequest) {
            requestDriverNow();
        } else if (backoff) {
            schedulePumpWait(backoffMs);
        }
    }

    /// 下一轮驱动的决策 (调用方持 [mutex_]; 不在此处调用主程序接口)。
    enum class PumpDecision {
        Stop,      ///< 不轮询: 无未结束的 polled 操作, 或已停止
        Immediate, ///< 有进展: 立即申请下一次请求 (不等退避)
        Backoff,   ///< 无进展或达到突发上限: 安排一次退避后再驱动
    };

    /// 受控轮询的调度策略 (调用方持 [mutex_])。
    ///
    /// - 无未结束的 polled 操作 (`polledRoots_ == 0`): 不轮询, 突发计数归零;
    /// - 本轮 `poll_one` 执行到了 handler (有进展) 且未达突发上限: 立即续票,
    ///   等待中的网络/子进程/文件就能在就绪的下一个瞬间被处理 (最坏延迟 = 退避时间片);
    /// - 否则: 安排一次退避 (无进展 = `kPollIntervalMs`; 达到突发上限 = 让出
    ///   `kPollBurstYieldMs` 给主程序与同实例其它操作), 已有已排定的退避时不重复安排。
    PumpDecision pumpNextLocked(std::size_t progressed, int64_t& backoffMs) noexcept {
        backoffMs = 0;
        if (stopping_ || polledRoots_.load(std::memory_order_acquire) == 0) {
            pollBurst_ = 0;
            return PumpDecision::Stop;
        }
        const bool advanced = progressed > 0;
        if (advanced && pollBurst_ < kPollBurstMax) {
            ++pollBurst_;
            pumpPending_ = true;
            return PumpDecision::Immediate;
        }
        if (advanced) {
            // 突发上限: 连续快进后强制让出, 避免同实例自循环长期独占 IO 线程。
            pollBurst_ = 0;
            backoffMs  = kPollBurstYieldMs;
        } else {
            pollBurst_ = 0;
            backoffMs  = kPollIntervalMs;
        }
        if (pumpWaitScheduled_) {
            return PumpDecision::Stop; // 已有已排定的退避: 到点后会自己续票
        }
        pumpWaitScheduled_ = true;
        idlePollCount_.fetch_add(1, std::memory_order_acq_rel);
        return PumpDecision::Backoff;
    }

    /// 安排一次"退避后驱动" (主程序计时器适配; 只能在主程序 IO 线程调用)。
    ///
    /// - 到期回调 [pumpWaitDone] 只做"请求下一次请求", 不恢复插件协程;
    /// - 必须在 [mutex_] 之外调用主程序 `sleep` (回调可能同步到达);
    /// - 主程序计时器不可用时无法退避: 记一次错误并终结未结束的 polled 根,
    ///   避免"根永远不被推进"这类静默悬挂。
    void schedulePumpWait(int64_t ms) noexcept {
        if (!sched_ || !sched_->sleep) {
            {
                std::lock_guard lock(mutex_);
                pumpWaitScheduled_ = false;
            }
            logError("polled coroutine needs the host scheduler timer, but it is unavailable: "
                     "terminating the polled operations instead of hanging");
            failAllPolledRoots("host scheduler timer unavailable for polled coroutine");
            return;
        }
        PluginxxString error{nullptr, 0};
        auto*          op = sched_->sleep(host_, ms, &PollOneBridge::pumpWaitDone, this, &error);
        if (!op) {
            std::string message = "scheduler timer rejected";
            if (error.data) {
                message.assign(error.data, static_cast<size_t>(error.size));
                PluginString::free(host_, &error);
            }
            {
                std::lock_guard lock(mutex_);
                pumpWaitScheduled_ = false;
            }
            logError(fmt::format("polled coroutine backoff timer failed: {}", message));
            failAllPolledRoots(message);
            return;
        }
        bool keep = false;
        {
            std::lock_guard lock(mutex_);
            // 回调可能在 sleep 返回前同步到达 (立即取消): 此时它已把
            // pumpWaitScheduled_ 清掉, 句柄收束完毕, 这里只需取消该请求。
            if (pumpWaitScheduled_ && !stopping_) {
                pumpWaitOp_ = op;
                keep        = true;
            }
        }
        if (!keep) {
            cancelSchedulerOp(op);
        }
    }

    /// 退避定时器到期 (或被 `op_cancel` 取消) 的回调: 请求下一次请求。
    ///
    /// 被取消也算"退避结束" —— `execute_cancel` 正是靠取消退避来让插件立刻获得一次
    /// 驱动, 从而不必等满整个退避时间片才看到取消。
    static void PLUGINXX_CALL
        pumpWaitDone(void* ud, int32_t status, const PluginxxStringView* payload) noexcept {
        (void)status;
        (void)payload;
        auto* self = static_cast<PollOneBridge*>(ud);
        if (!self) {
            return;
        }
        bool needRequest = false;
        {
            std::lock_guard lock(self->mutex_);
            self->pumpWaitScheduled_ = false;
            self->pumpWaitOp_        = nullptr;
            if (!self->stopping_ && self->polledRoots_.load(std::memory_order_acquire) > 0) {
                self->pumpPending_ = true;
                needRequest        = self->prepareScheduleLocked();
            }
        }
        if (needRequest) {
            self->requestDriverNow();
        }
    }

    /// 取出已排定的退避句柄并复位调度状态 (调用方持 [mutex_]; 取消动作在锁外执行)。
    PluginxxOperatorHandle* takePumpWaitLocked() noexcept {
        auto* op           = pumpWaitOp_;
        pumpWaitOp_        = nullptr;
        pumpWaitScheduled_ = false;
        return op;
    }

    /// 取消一个主程序调度句柄 (幂等; 已收束的句柄由主程序按空操作处理)。
    void cancelSchedulerOp(PluginxxOperatorHandle* op) noexcept {
        if (op && cancel_ && cancel_->op_cancel) {
            cancel_->op_cancel(op);
        }
    }

    /// 状态机锁: 保护下面前五组状态 (wake 可从任意线程调用)。
    /// `mutable` 是为了让只读诊断方法 (hasPendingWake 等) 也能加锁。
    mutable std::mutex                                         mutex_;
    asio::io_context                                           localIo_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    const PluginxxHost*           host_    = nullptr;
    /// 主程序调度与驱动表 (`request_driver` / `cancel_driver` / `sleep` 退避 + 取消)
    const PluginxxSchedulerIface* sched_   = nullptr;
    /// 异步操作句柄的唯一取消入口 (`cancel.op_cancel`)
    const PluginxxCancelIface*    cancel_  = nullptr;

    bool            stopping_      = false;
    bool            wakePending_   = false;
    bool            driverQueued_  = false;
    bool            driverRunning_ = false;
    PluginxxDriver* driver_        = nullptr;
    uint64_t        nextEpoch_     = 0;
    uint64_t        pendingEpoch_  = 0;

    /// 受控轮询的下一次请求理由 (见 [pumpNextLocked]/[prepareScheduleLocked])。
    bool pumpPending_ = false;
    /// 已排定的退避定时器 (调度状态与其句柄同属 [mutex_])。
    bool                    pumpWaitScheduled_ = false;
    PluginxxOperatorHandle* pumpWaitOp_        = nullptr;
    /// 连续"有进展"步数 (受控轮询突发计数)。
    int pollBurst_ = 0;

    std::atomic<bool> ranOnIoThread_{false};
    /// 已投递但尚未执行的本地步骤数 (见 [postToLocal] 与文件头的状态机说明)。
    std::atomic<uint64_t> readySteps_{0};
    std::atomic<uint64_t> ticketsIssued_{0};
    std::atomic<uint64_t> driverSteps_{0};
    /// 未结束的受控轮询操作数 (与 `polledRootList_` 同步; 读端不加锁)。
    std::atomic<uint64_t> polledRoots_{0};
    /// 已发生的"无进展退避"次数 (诊断)。
    std::atomic<uint64_t> idlePollCount_{0};
    int                   noProgressStreak_ = 0;

    /// 根登记表用的锁。**锁序**: 允许 `rootsMutex_` → `mutex_` (见
    /// [removePolledRoot]), 反向嵌套不存在, 因此不会死锁。
    mutable std::mutex rootsMutex_;
    /// 未结束的受控轮询根 (与 `polledRoots_` 计数一致)。
    std::vector<std::shared_ptr<PolledRoot>> polledRootList_;
    /// 被放弃的受控轮询根留下的清理回调 (回收 Job); 桥销毁时执行。
    std::vector<std::function<void()>> abandonedCleanups_;
};

} // namespace detail

/* ==================== 作用域引用的值类型 (ScopeRef) ==================== */

/// 主程序作用域引用的**值类型**: 自己持有 id 字符串 + 序号
///
/// - 用于插件把"我挂在哪个作用域上"存进按作用域划分的状态, 之后任何线程都能直接用;
/// - [raw] 返回借用视图 (指向本对象内部), 因此只能在本对象存活期间使用;
/// - 可拷贝 / 可比较 / 可排序 (日志与诊断)。
class ScopeRef {
public:

    ScopeRef() = default;

    ScopeRef(std::string id, uint64_t seq) :
        id_(std::move(id)),
        seq_(seq) {}

    std::string_view id() const noexcept {
        return id_;
    }

    uint64_t seq() const noexcept {
        return seq_;
    }

    bool empty() const noexcept {
        return id_.empty();
    }

    /// 传给接口表的借用视图 (指向本对象内部, 不要长期保存)
    PluginxxScopeRef raw() const noexcept {
        PluginxxScopeRef ref{};
        ref.struct_size = static_cast<uint32_t>(sizeof(PluginxxScopeRef));
        ref.flags       = 0;
        ref.id          = PluginStringView::from(id_.data(), id_.size());
        ref.seq         = seq_;
        return ref;
    }

    /// 稳定借用的视图指针 (缓存在本对象里, 有效期同本对象)
    /// - 方便把引用传给需要 `const PluginxxScopeRef*` 的调用点而不必每次建临时量
    const PluginxxScopeRef* ptr() const noexcept {
        cache_    = raw();
        cache_.id = PluginStringView::from(id_.data(), id_.size());
        return &cache_;
    }

    friend bool operator==(const ScopeRef& a, const ScopeRef& b) noexcept {
        return a.seq_ == b.seq_ && a.id_ == b.id_;
    }

    friend bool operator<(const ScopeRef& a, const ScopeRef& b) noexcept {
        if (a.id_ != b.id_) {
            return a.id_ < b.id_;
        }
        return a.seq_ < b.seq_;
    }

private:

    std::string id_;
    uint64_t    seq_ = 0;
    /// [ptr] 的缓存 (mutable: ptr() 是 const)
    mutable PluginxxScopeRef cache_{};
};

/* ==================== 插件框架事件驱动取消注册表 (CancelRegistry) ==================== */

/// 插件框架事件驱动取消注册表
/// - 职责: 按作用域键登记与操作级登记的取消事件注册、注销与原子通知
/// - 线程安全: 完全支持多线程并发注册、注销与触发
/// - 内存自治: 纯堆内存实例，无任何全局/静态状态，严格契合多实例契约
/// - 零悬挂保证: unregisterCallback
/// 会等待并排他锁定正在执行中的回调，确保调用栈上的对象不会在回调执行中析构
class CancelRegistry {
public:

    using CancelCallback = std::function<void()>;
    using RegId          = uint64_t;

    /// 回调实体封装：支持并发排他保护与生命周期状态标记
    struct CallbackEntry {
        std::recursive_mutex mu;
        bool                 disposed{false};
        CancelCallback       cb;
    };

    CancelRegistry() = default;

    ~CancelRegistry() {
        cancelAll();
    }

    CancelRegistry(const CancelRegistry&)            = delete;
    CancelRegistry& operator=(const CancelRegistry&) = delete;

    /// 注册取消回调
    /// - `key`: 作用域内的键 (由主程序定义, 如会话 / 房间 / 请求)，为空表示不绑定具体键的独立操作
    /// - `cb`: 取消触发时的回调动作
    /// - `return`: 注册凭证 ID (0 表示由于已经处于取消态而直接同步触发，无需反注册)
    RegId registerCallback(std::string_view key, CancelCallback cb) {
        if (!cb) {
            return 0;
        }
        auto entry = std::make_shared<CallbackEntry>();
        entry->cb  = std::move(cb);

        RegId id               = nextId_.fetch_add(1, std::memory_order_relaxed);
        bool  alreadyCanceled = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!key.empty() && canceledKeys_.count(std::string(key)) > 0) {
                alreadyCanceled = true;
            } else {
                entries_[id] = entry;
                if (!key.empty()) {
                    keyToIds_[std::string(key)].push_back(id);
                    idToKey_[id] = std::string(key);
                }
            }
        }

        // 若该 key 之前已由主程序下发过取消，锁外直接同步执行回调
        if (alreadyCanceled) {
            std::lock_guard<std::recursive_mutex> elock(entry->mu);
            if (entry->cb) {
                try {
                    entry->cb();
                } catch (...) {
                }
            }
            return 0;
        }
        return id;
    }

    /// 注销回调 (命令/操作正常结束退出作用域时调用)
    /// - 排他性防悬挂保证: 若此时 cancel 正在另一线程执行该回调，elock
    /// 会阻塞等待其执行完毕，避免回调访问已被销毁的对象
    void unregisterCallback(RegId id) {
        if (id == 0) {
            return;
        }
        std::shared_ptr<CallbackEntry> entry;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto                        it = entries_.find(id);
            if (it != entries_.end()) {
                entry = std::move(it->second);
                entries_.erase(it);
            } else {
                auto ait = activeInvocations_.find(id);
                if (ait != activeInvocations_.end()) {
                    entry = ait->second;
                }
            }
            auto kit = idToKey_.find(id);
            if (kit != idToKey_.end()) {
                auto vkit = keyToIds_.find(kit->second);
                if (vkit != keyToIds_.end()) {
                    auto& vec = vkit->second;
                    vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
                    if (vec.empty()) {
                        keyToIds_.erase(vkit);
                    }
                }
                idToKey_.erase(kit);
            }
        }
        if (entry) {
            std::lock_guard<std::recursive_mutex> elock(entry->mu);
            entry->disposed = true;
            entry->cb       = nullptr;
        }
    }

    /// 触发指定 key 的取消通知
    /// - 将 key 标记为已取消；提取该 key 下所有未注销的回调并在全局锁外安全执行
    void cancel(std::string_view key) {
        if (key.empty()) {
            return;
        }
        std::vector<std::pair<RegId, std::shared_ptr<CallbackEntry>>> toInvoke;
        {
            std::lock_guard<std::mutex> lock(mu_);
            canceledKeys_.insert(std::string(key));
            auto kit = keyToIds_.find(std::string(key));
            if (kit != keyToIds_.end()) {
                for (RegId id : kit->second) {
                    auto eit = entries_.find(id);
                    if (eit != entries_.end()) {
                        toInvoke.push_back({id, eit->second});
                        activeInvocations_[id] = eit->second;
                        entries_.erase(eit);
                    }
                    idToKey_.erase(id);
                }
                keyToIds_.erase(kit);
            }
        }
        // 在全局锁外执行回调，避免回调内部加锁导致死锁
        for (auto& [id, entry] : toInvoke) {
            {
                std::lock_guard<std::recursive_mutex> elock(entry->mu);
                if (!entry->disposed && entry->cb) {
                    try {
                        entry->cb();
                    } catch (...) {
                    }
                    entry->cb = nullptr;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                activeInvocations_.erase(id);
            }
        }
    }

    /// 触发所有正在运行任务的取消 (插件卸载或实例销毁时补充调用)
    void cancelAll() {
        std::vector<std::pair<RegId, std::shared_ptr<CallbackEntry>>> toInvoke;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [id, entry] : entries_) {
                toInvoke.push_back({id, entry});
                activeInvocations_[id] = entry;
            }
            entries_.clear();
            keyToIds_.clear();
            idToKey_.clear();
        }
        for (auto& [id, entry] : toInvoke) {
            {
                std::lock_guard<std::recursive_mutex> elock(entry->mu);
                if (!entry->disposed && entry->cb) {
                    try {
                        entry->cb();
                    } catch (...) {
                    }
                    entry->cb = nullptr;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                activeInvocations_.erase(id);
            }
        }
    }

    /// 查询指定 key 是否已被标记取消
    /// - 纯内存无跨线程调用，避免向主程序 IO 线程高频查询
    bool isCanceled(std::string_view key) const {
        if (key.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        return canceledKeys_.count(std::string(key)) > 0;
    }

    /// 重置指定 key 的取消标记 (新轮次开始时可选调用)
    void clearCanceled(std::string_view key) {
        if (key.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        canceledKeys_.erase(std::string(key));
    }

    /// 查询当前注册的活跃回调总数
    size_t activeCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.size();
    }

    /// RAII 守卫：离开作用域自动安全注销
    class [[nodiscard]] ScopedRegistration {
    public:

        ScopedRegistration() = default;

        ScopedRegistration(CancelRegistry* reg, RegId id) :
            reg_(reg),
            id_(id) {}

        ~ScopedRegistration() {
            if (reg_ && id_ != 0) {
                reg_->unregisterCallback(id_);
            }
        }

        ScopedRegistration(ScopedRegistration&& o) noexcept :
            reg_(o.reg_),
            id_(o.id_) {
            o.reg_ = nullptr;
            o.id_  = 0;
        }

        ScopedRegistration& operator=(ScopedRegistration&& o) noexcept {
            if (this != &o) {
                if (reg_ && id_ != 0) {
                    reg_->unregisterCallback(id_);
                }
                reg_   = o.reg_;
                id_    = o.id_;
                o.reg_ = nullptr;
                o.id_  = 0;
            }
            return *this;
        }

        ScopedRegistration(const ScopedRegistration&)            = delete;
        ScopedRegistration& operator=(const ScopedRegistration&) = delete;

        RegId id() const noexcept {
            return id_;
        }

        void release() noexcept {
            reg_ = nullptr;
            id_  = 0;
        }

    private:

        CancelRegistry* reg_ = nullptr;
        RegId           id_  = 0;
    };

    /// 快捷绑定接口，返回 ScopedRegistration
    ScopedRegistration bind(std::string_view key, CancelCallback cb) {
        RegId id = registerCallback(key, std::move(cb));
        return ScopedRegistration(this, id);
    }

private:

    mutable std::mutex                                        mu_;
    std::atomic<RegId>                                        nextId_{1};
    std::unordered_map<RegId, std::shared_ptr<CallbackEntry>> entries_;
    std::unordered_map<RegId, std::shared_ptr<CallbackEntry>> activeInvocations_;
    std::unordered_map<std::string, std::vector<RegId>>       keyToIds_;
    std::unordered_map<RegId, std::string>                    idToKey_;
    std::unordered_set<std::string>                           canceledKeys_;
};

/* ==================== 操作控制对象 (OpCtl) ==================== */

struct OpCtl {
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    CancelRegistry*                    cancelRegistry = nullptr;
    /// 主程序定义的作用域内键 (如会话 / 房间 / 请求); 空 = 未绑定具体键
    std::string scopeKey;
    /// 本操作所属的作用域; 空 = 不标明归属
    ScopeRef scope;

    bool canceled() const noexcept {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            return true;
        }
        return cancelRegistry && !scopeKey.empty() && cancelRegistry->isCanceled(scopeKey);
    }

    void throw_if_canceled() const {
        if (canceled()) {
            throw CanceledException("operation canceled");
        }
    }
};

/* ==================== 强类型参数提取器 ArgReader ==================== */

namespace detail {
template<typename T>
inline T jsonGet(const utilxx_base::Json& j) {
    if constexpr (std::is_same_v<T, std::string>) {
        return j.get<std::string>();
    } else if constexpr (std::is_same_v<T, bool>) {
        return j.get<bool>();
    } else if constexpr (std::is_same_v<T, double>) {
        return j.get<double>();
    } else if constexpr (std::is_same_v<T, float>) {
        return j.get<float>();
    } else if constexpr (std::is_same_v<T, long long>) {
        return j.get<long long>();
    } else if constexpr (std::is_same_v<T, unsigned long long>) {
        return j.get<unsigned long long>();
    } else if constexpr (std::is_same_v<T, long>) {
        return j.get<long>();
    } else if constexpr (std::is_same_v<T, unsigned long>) {
        return j.get<unsigned long>();
    } else if constexpr (std::is_same_v<T, int>) {
        return j.get<int>();
    } else if constexpr (std::is_same_v<T, unsigned int>) {
        return j.get<unsigned>();
    } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
        return j.get<std::vector<std::string>>();
    } else if constexpr (std::is_same_v<T, utilxx_base::Json>) {
        return j.get<utilxx_base::Json>();
    } else {
        return j.get<T>();
    }
}

/// 参数文本里允许忽略的空白 (ASCII)
inline bool isArgSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/// 去掉首尾 ASCII 空白
inline std::string_view trimArgSpace(std::string_view s) noexcept {
    size_t b = 0, e = s.size();
    while (b < e && isArgSpace(s[b])) {
        ++b;
    }
    while (e > b && isArgSpace(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

/// 严格整数文本解析: 要求**整个**字符串 (允许首尾空白与一个前导 '+') 都是十进制整数
/// - 与 `std::stoll` 的区别: 不接受前缀匹配, `"100 lines"` 解析失败而不是得 100
/// - 溢出同样算失败 (不做回绕截断)
inline bool parseWholeInteger(std::string_view s, long long& out) {
    s = trimArgSpace(s);
    if (s.empty()) {
        return false;
    }
    if (s.front() == '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return false;
    }
    long long  v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) {
        return false;
    }
    out = v;
    return true;
}

/// 严格浮点文本解析: 要求**整个**字符串 (允许首尾空白与一个前导 '+') 都是十进制数值
/// - 不接受前缀匹配 (`"1.5 kg"` 解析失败), 也不接受 `0x` 十六进制浮点写法
/// - `inf`/`nan`/溢出与下溢均算失败
inline bool parseWholeDouble(std::string_view s, double& out) {
    s = trimArgSpace(s);
    if (s.empty()) {
        return false;
    }
    // 拒绝 0x 十六进制浮点 (strtod 支持 "0x1p3" 这类写法, 但参数里出现它多半是模型写错)
    auto body = s;
    if (body.front() == '+' || body.front() == '-') {
        body.remove_prefix(1);
    }
    if (body.size() >= 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
        return false;
    }
    std::string tmp(s);
    char*       end = nullptr;
    errno           = 0;
    const double v  = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size() || errno != 0 || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

/// 布尔文本解析 (忽略首尾空白与 ASCII 大小写)
/// - 真: `true` / `yes` / `y` / `on` / `enable` / `enabled` / `1`
/// - 假: `false` / `no` / `n` / `off` / `disable` / `disabled` / `0`
/// - 其它文本 (如 `"ok"`) 返回 false 表示“读不到”: 调用方的
///   `value(k, fallback)` 用默认值、`require<bool>` 报参数错误, 不再静默当成 false
inline bool parseBoolText(std::string_view s, bool& out) {
    s = trimArgSpace(s);
    if (s.empty()) {
        return false;
    }
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) {
        lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c);
    }
    if (lower == "true" || lower == "yes" || lower == "y" || lower == "on"
        || lower == "enable" || lower == "enabled" || lower == "1") {
        out = true;
        return true;
    }
    if (lower == "false" || lower == "no" || lower == "n" || lower == "off"
        || lower == "disable" || lower == "disabled" || lower == "0") {
        out = false;
        return true;
    }
    return false;
}

/// 浮点 JSON 值 -> 整数目标的取整
/// - 与向上取整相差不超过 1e-6 时取向上取整 (4.999999999 -> 5), 否则向下取整
///   (3.7 -> 3, -3.7 -> -4)
/// - NaN/Inf 与超出 long long 表示范围时返回 false
inline bool doubleToWholeInteger(double v, long long& out) {
    if (!std::isfinite(v)) {
        return false;
    }
    constexpr double kRoundEps = 1e-6;
    const auto       up        = std::ceil(v);
    const double     rounded   = (up - v <= kRoundEps) ? up : std::floor(v);
    constexpr double kInt64Min   = -9223372036854775808.0;
    constexpr double kInt64MaxP1 = 9223372036854775808.0;
    if (!(rounded >= kInt64Min && rounded < kInt64MaxP1)) {
        return false;
    }
    out = static_cast<long long>(rounded);
    return true;
}

/// long long 能否无损放进 T (整数目标类型): 越界时不做强制转换 (避免静默回绕)
template<typename T>
inline bool fitsInType(long long v) {
    if constexpr (std::is_signed_v<T>) {
        return v >= static_cast<long long>(std::numeric_limits<T>::min())
               && v <= static_cast<long long>(std::numeric_limits<T>::max());
    } else {
        return v >= 0
               && static_cast<unsigned long long>(v)
                      <= static_cast<unsigned long long>(std::numeric_limits<T>::max());
    }
}
} // namespace detail

/// 强类型参数提取器: 把工具参数 JSON 按调用方要的类型读出来
///
/// 读取时的转换规则 (参数本身不会被改写):
/// - 字符串 -> 整数/浮点: **整个**字符串都必须是数值 (允许首尾空白与一个前导 '+'),
///   不接受前缀匹配 (`"100 lines"` 读不到整数); 整数还要落在目标类型范围内
/// - 字符串 -> 布尔: 忽略大小写与首尾空白, `true`/`yes`/`y`/`on`/`enable`/`enabled`/`1`
///   为真, `false`/`no`/`n`/`off`/`disable`/`disabled`/`0` 为假; 其它文本“读不到”
///   (调用方的 `value(k, fallback)` 用默认值、`require<T>` 报参数错误)
/// - 布尔/数值 -> 其它数值: 按 C++ 隐式转换读; 浮点 -> 整数取“相近整数”
///   (与向上取整相差不超过 1e-6 时向上取整, 否则向下取整: 3.7 -> 3, -3.7 -> -4), 越界不读
/// - 任意值 -> 字符串: 非字符串按 `dump()` 文本返回
/// - 单字符串 -> `std::vector<std::string>`: 包成单元素列表; 数组逐项取 (非字符串项 `dump()`)
///
/// 读不到时 `get` 返回 `std::nullopt`、`require` 记一条参数错误 (`ok()` 为 false),
/// 都不会抛异常出 SDK 边界。
class ArgReader {
public:

    explicit ArgReader(std::string_view jsonStr) {
        if (!jsonStr.empty()) {
            try {
                root_ = utilxx_base::Json::parse(jsonStr);
                if (!root_.is_object()) {
                    root_ = utilxx_base::Json::object();
                }
            } catch (...) {
                hasParseError_ = true;
                root_          = utilxx_base::Json::object();
            }
        } else {
            root_ = utilxx_base::Json::object();
        }
    }

    bool hasParseError() const noexcept {
        return hasParseError_;
    }

    template<typename T>
    std::optional<T> get(std::string_view key) const {
        if (hasParseError_ || !root_.is_object()) {
            return std::nullopt;
        }
        if (!root_.contains(key)) {
            return std::nullopt;
        }
        auto val = root_[key];
        if (val.is_null()) {
            return std::nullopt;
        }

        try {
            if constexpr (std::is_same_v<T, utilxx_base::Json>) {
                return val;
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (val.is_string()) {
                    return detail::jsonGet<std::string>(val);
                }
                return val.dump();
            } else if constexpr (std::is_same_v<T, bool>) {
                if (val.is_boolean()) {
                    return detail::jsonGet<bool>(val);
                }
                if (val.is_number()) {
                    return detail::jsonGet<long long>(val) != 0;
                }
                if (val.is_string()) {
                    // 忽略大小写认 true/false/1/0; 其它文本视为"读不到"
                    // (以前一律当 false, 会把 "True"/"enable" 静默变成 false)
                    bool b = false;
                    if (detail::parseBoolText(detail::jsonGet<std::string>(val), b)) {
                        return b;
                    }
                }
            } else if constexpr (std::is_integral_v<T>) {
                if (val.is_number_integer()) {
                    const auto v = detail::jsonGet<long long>(val);
                    if (!detail::fitsInType<T>(v)) {
                        return std::nullopt;
                    }
                    return static_cast<T>(v);
                }
                if (val.is_number()) {
                    // 浮点值 -> 整数: 取相近整数 (3.7 -> 3, 4.999999999 -> 5),
                    // 越界/NaN 视为读不到 (以前是 static_cast 直接截断/回绕)
                    long long rounded = 0;
                    if (!detail::doubleToWholeInteger(detail::jsonGet<double>(val), rounded)
                        || !detail::fitsInType<T>(rounded)) {
                        return std::nullopt;
                    }
                    return static_cast<T>(rounded);
                }
                if (val.is_string()) {
                    // 严格解析: 整个字符串都必须是整数, 且要落在 T 的取值范围内
                    // (以前用 std::stoll, "100 lines" 会解析成 100)
                    long long  parsed = 0;
                    const auto s      = detail::jsonGet<std::string>(val);
                    if (!detail::parseWholeInteger(s, parsed) || !detail::fitsInType<T>(parsed)) {
                        return std::nullopt;
                    }
                    return static_cast<T>(parsed);
                }
            } else if constexpr (std::is_floating_point_v<T>) {
                if (val.is_number()) {
                    return static_cast<T>(detail::jsonGet<double>(val));
                }
                if (val.is_string()) {
                    // 严格解析: 整个字符串都必须是数值 (不允许 "1.5 kg" 这类前缀匹配)
                    double parsed = 0;
                    if (!detail::parseWholeDouble(detail::jsonGet<std::string>(val), parsed)) {
                        return std::nullopt;
                    }
                    const auto out = static_cast<T>(parsed);
                    if (!std::isfinite(static_cast<double>(out))) {
                        return std::nullopt;
                    }
                    return out;
                }
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                if (val.is_array()) {
                    std::vector<std::string> res;
                    for (const auto& elem : val) {
                        if (elem.is_string()) {
                            res.push_back(detail::jsonGet<std::string>(elem));
                        } else {
                            res.push_back(elem.dump());
                        }
                    }
                    return res;
                }
                if (val.is_string()) {
                    return std::vector<std::string>{detail::jsonGet<std::string>(val)};
                }
            } else {
                return detail::jsonGet<T>(val);
            }
        } catch (...) {
            return std::nullopt;
        }
        return std::nullopt;
    }

    template<typename T>
    T value(std::string_view key, const T& fallback) const {
        auto opt = get<T>(key);
        return opt.has_value() ? *opt : fallback;
    }

    std::string value(std::string_view key, const char* fallback) const {
        auto opt = get<std::string>(key);
        return opt.has_value() ? *opt : std::string(fallback ? fallback : "");
    }

    template<typename T>
    T require(std::string_view key) {
        auto opt = get<T>(key);
        if (!opt.has_value()) {
            errors_.push_back(fmt::format("Missing or invalid required argument: '{}'", key));
            return T{};
        }
        return *opt;
    }

    bool ok() const noexcept {
        return errors_.empty() && !hasParseError_;
    }

    std::string errorMessage() const {
        if (hasParseError_) {
            return "Failed to parse arguments JSON";
        }
        if (errors_.empty()) {
            return {};
        }
        return fmt::format("Argument error: {}", fmt::join(errors_, "; "));
    }

    const utilxx_base::Json& raw() const noexcept {
        return root_;
    }

private:

    utilxx_base::Json        root_          = utilxx_base::Json::object();
    bool                     hasParseError_ = false;
    std::vector<std::string> errors_;
};
/* ==================== 根操作 Request / 完成与生命周期协议 ==================== */

namespace detail {

/* ==================== 桥接 (host driver) 路径的公共辅助 ==================== */

/* ==================== 根操作 Request 与完成守卫 ====================
 *
 * tool / hook / capability / graph / spawn 这些"根操作"共享同一套协议：
 * 主程序把 args/session/call_id/method 以**只读借用视图**传入 start，插件必须在
 * 本次调用内复制需要跨挂起保留的数据；接受之后必须 exactly-once 完成。
 * 下面两个类型把这套协议集中在一处，避免每个 helper 各写一遍。
 */

/// 根操作拥有型输入。
///
/// - C ABI 传入的视图只在 start 调用期间有效，本结构先把它们复制成自己的
///   `std::string`，业务代码拿到的 `std::string_view` 因此在整个根操作期间有效；
/// - 同步路径的 Request 在栈上、异步路径的 Request 在 Job 里持有，随完成回调
///   一起释放；
/// - 业务需要跨操作保留时仍要自己复制（Request 之外不保证）。
struct RootRequest {
    std::string argsJson; ///< 操作参数 JSON（空则为 "{}"）
    std::string scopeKey; ///< 主程序定义的作用域内键 (ABI 的 scope_key)
    std::string callId; ///< 操作标识（工具调用 id / 请求 id；语义由主程序定义）
    std::string method; ///< 能力方法名（非能力操作为空）

    const PluginxxHost*        host        = nullptr;
    const PluginxxCancelToken* cancelToken = nullptr; ///< offload/worker 内的取消令牌视图

    /// 主程序自定义字段（键 → 值）: 框架不解释, 只在根操作期间原样携带。
    /// 供主程序把自己的操作种类细节（节点名 / 只读快照 / 目标对象 ...）带给业务代码。
    std::vector<std::pair<std::string, std::string>> extraFields;

    static std::string copyView(const PluginxxStringView* sv, const char* fallback = "") {
        if (!sv || (!sv->data && sv->size == 0)) {
            return std::string{fallback};
        }
        return std::string(sv->data ? sv->data : "", static_cast<size_t>(sv->size));
    }

    /// 能力：method/args（唯一的框架内置根操作: 能力 RPC 本身与业务无关）
    static RootRequest forCapability(
        const PluginxxHost*       host,
        const PluginxxStringView* method,
        const PluginxxStringView* args
    ) {
        RootRequest req;
        req.host     = host;
        req.method   = copyView(method);
        req.argsJson = copyView(args, "{}");
        return req;
    }

    /// 设置主程序自定义字段（同名键覆盖）
    RootRequest& setExtra(std::string key, std::string value) {
        for (auto& [k, v] : extraFields) {
            if (k == key) {
                v = std::move(value);
                return *this;
            }
        }
        extraFields.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    /// 读取主程序自定义字段（不存在返回空串视图；视图指向本对象内部）
    std::string_view extra(std::string_view key) const noexcept {
        for (const auto& [k, v] : extraFields) {
            if (k == key) {
                return v;
            }
        }
        return {};
    }

    std::string_view args() const noexcept {
        return argsJson;
    }

    std::string_view scope() const noexcept {
        return scopeKey;
    }

    std::string_view call() const noexcept {
        return callId;
    }

    std::string_view capMethod() const noexcept {
        return method;
    }
};

/// 根操作完成守卫：保证 notify.done 恰好一次，并把异常统一映射为终态。
///
/// 规则（plugin.md 不可变原则 5）：
/// - 业务代码显式调用 ok()/failed()/canceled() 表示完成；
/// - 作用域结束时仍未完成（提前 return 等）时析构补一次 FAILED，绝不留下
///   "已接受但永远不 done"的操作；
/// - 重复完成只记录，不重复回调主程序。
///
/// 异步路径（asio 协程）在各自的收尾处做同一件事:
/// 先 claim 一次终态, 再回调主程序 (见 [detail::PolledRoot] / [detail::ProviderRoot])。
class CompletionGuard {
public:

    explicit CompletionGuard(const PluginxxOperatorNotify* notify) :
        notify_(notify ? *notify : PluginxxOperatorNotify{nullptr, nullptr}) {}

    CompletionGuard(const CompletionGuard&)            = delete;
    CompletionGuard& operator=(const CompletionGuard&) = delete;

    bool completed() const noexcept {
        return done_.load(std::memory_order_acquire);
    }

    /// 完成一次（`status` 为 PLUGINXX_OPERATOR_*）；重复调用返回 false。
    bool complete(int32_t status, std::string_view payload) noexcept {
        if (!notify_.done) {
            done_.store(true, std::memory_order_release);
            return false;
        }
        bool expected = false;
        if (!done_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        try {
            auto sv = PluginStringView::from(payload.data(), payload.size());
            notify_.done(notify_.host_ud, status, &sv);
        } catch (...) {
            // 主程序回调不得抛异常；即便抛了也已经置位，不重复派发。
        }
        return true;
    }

    void ok(std::string_view payload = {}) {
        complete(PLUGINXX_OPERATOR_OK, payload);
    }

    void failed(std::string_view error) {
        complete(PLUGINXX_OPERATOR_FAILED, error);
    }

    void canceled(std::string_view reason = {}) {
        complete(PLUGINXX_OPERATOR_CANCELED, reason);
    }

    /// 在 catch 块中调用：把当前异常映射为终态（取消异常映射为 Canceled）。
    void fromCurrentException() noexcept {
        try {
            throw;
        } catch (const CanceledException& e) {
            canceled(e.what());
        } catch (const std::exception& e) {
            failed(e.what());
        } catch (...) {
            failed("unknown plugin error");
        }
    }

    ~CompletionGuard() {
        if (!completed()) {
            complete(PLUGINXX_OPERATOR_FAILED, "plugin did not complete the operation");
        }
    }

private:

    PluginxxOperatorNotify notify_{nullptr, nullptr};
    std::atomic<bool>      done_{false};
};

/// 生命周期入口 (`start` / `mount` / `stop` / `unmount`) 的"稍后完成"凭据
///
/// 生命周期的四个入口都是"受理 + 完成后通知"的契约: 返回 `NULL` 表示已受理,
/// 结果稍后经 `PluginxxOperatorNotify::done` 交回。本类把"交回一次"这件事包起来:
/// - 入口里 `auto later = finishLater(notify);` 拿到凭据;
/// - 需要等表调用时把它交给一条协程 (`ctx.spawn(...)`) 或后台任务, 入口立刻返回;
/// - 协程里 `co_await` 完注册类调用后调 `later.ok()` / `failed(原因)`;
/// - 凭据可以复制 (共享同一个 once 标记), 全部销毁时仍未完成 -> 自动交 `FAILED`,
///   不会让主程序永远等下去。
///
/// 典型写法:
/// ```cpp
/// void* start(void* ud, const PluginxxOperatorNotify* notify, PluginxxString*) {
///     auto& ctx = *static_cast<MyCtx*>(ud);
///     auto  later = pluginxx::finishLater(notify);
///     ctx.spawn([later](MyCtx& c) -> asio::awaitable<void> {
///         co_await c.registerTool(...);      // 表调用, 不阻塞 IO 线程
///         later.ok();
///         co_return;
///     });
///     return nullptr;                        // 已受理, 完成时再通知
/// }
/// ```
/// 生命周期"稍后完成"的堆上状态 (主程序拿到的 provider 句柄就是它的地址)
///
/// 为什么要一个堆对象: 协议要求"入口返回非空句柄"才算已受理, 只有 `notify.done`
/// 后到才算异步 —— 返回 `NULL` 且尚未 done 会被判成协议违约
/// (见 `pluginxx/runtime/op_driver.h` 的 `PluginOperation::start`)。
struct LifecycleJob {
    explicit LifecycleJob(const PluginxxOperatorNotify* notify) :
        guard(notify) {}

    /// 完成守卫: 恰好一次, 全部副本销毁仍未完成则自动 FAILED
    CompletionGuard guard;
};

/// 生命周期入口 (`start` / `mount` / `stop` / `unmount`) 的"稍后完成"凭据
///
/// 四个入口都是"返回句柄 = 已受理, 完成后经 `PluginxxOperatorNotify::done` 交回"
/// 的契约, 但**异步完成必须返回非空句柄**: 返回 `NULL` 且还没调用 `done` 会被主程序
/// 判成协议违约。本类把这两件事包在一起:
///
/// - 入口里 `auto later = finishLater(notify);` 拿凭据;
/// - 需要等表调用时把它交给一条协程 (`ctx.spawn(...)`) 或后台任务, 入口
///   `return later.provider();` 立刻放行;
/// - 协程里 `co_await` 完注册类调用后调 `later.ok()` / `failed(原因)`;
/// - 凭据可复制 (共享同一份状态), 全部销毁时仍未完成 -> 自动交 `FAILED`,
///   不会让主程序永远等下去。
///
/// 典型写法:
/// ```cpp
/// void* start(void* ud, const PluginxxOperatorNotify* notify, PluginxxString*) {
///     auto& ctx = *static_cast<MyCtx*>(ud);
///     auto  later = pluginxx::finishLater(notify);
///     if (!later.active()) { return nullptr; }          // 主程序没要求通知
///     ctx.spawn([later](MyCtx& c, OpCtl&) -> asio::awaitable<void> {
///         co_await c.registerTool(...);                  // 表调用, 不阻塞 IO 线程
///         later.ok();
///         co_return;
///     });
///     return later.provider();                           // 已受理, 完成时再通知
/// }
/// ```
class LifecycleFinisher {
public:

    LifecycleFinisher() = default;

    /// 由 [finishLater] 构造 (`notify` 可空: 主程序没要求通知时全部方法都是空操作)
    explicit LifecycleFinisher(const PluginxxOperatorNotify* notify) {
        if (notify && notify->done) {
            job_ = std::make_shared<LifecycleJob>(notify);
        }
    }

    /// 是否绑定了完成回调 (false = 主程序没要求通知, 入口直接用 `NULL` 同步完成)
    bool active() const noexcept {
        return job_ != nullptr;
    }

    /// 交给主程序的 provider 句柄 (非空 = 已受理; `active()` 为 false 时是空)
    void* provider() const noexcept {
        return job_.get();
    }

    /// 已经交回结果
    bool completed() const noexcept {
        return job_ && job_->guard.completed();
    }

    void ok(std::string_view payload = {}) const noexcept {
        if (job_) {
            job_->guard.ok(payload);
        }
    }

    void failed(std::string_view error) const noexcept {
        if (job_) {
            job_->guard.failed(error);
        }
    }

    void canceled(std::string_view reason = {}) const noexcept {
        if (job_) {
            job_->guard.canceled(reason);
        }
    }

    /// 把当前异常映射成终态 (在 catch 块里调用; 取消异常 -> Canceled)
    void fromCurrentException() const noexcept {
        if (job_) {
            job_->guard.fromCurrentException();
        }
    }

private:

    std::shared_ptr<LifecycleJob> job_;
};

/// 取生命周期入口的"稍后完成"凭据 (见 [LifecycleFinisher])
inline LifecycleFinisher finishLater(const PluginxxOperatorNotify* notify) {
    return LifecycleFinisher{notify};
}

} // namespace detail

/// 根操作拥有型输入的公开别名：插件业务签名可直接写 `const RootRequest&`
/// （tool/hook/capability/graph 通用；异步路径由 Job 持有到协程结束）。
using RootRequest = detail::RootRequest;

/// 生命周期"稍后完成"凭据的公开别名 (见 [detail::LifecycleFinisher])
using LifecycleFinisher = detail::LifecycleFinisher;
using detail::finishLater;

/* ==================== 后台协作任务 spawn 实现 ==================== */

namespace detail {

/// 判断一个类型是不是 `asio::awaitable`
template<typename T>
struct IsPluginAwaitable : std::false_type {};

template<typename T, typename Executor>
struct IsPluginAwaitable<asio::awaitable<T, Executor>> : std::true_type {};

/// 把 `fn(ctx, ctl)` 的 `asio::awaitable<T>` 结果统一收成 `asio::awaitable<void>`
template<typename Ctx, typename Fn>
inline asio::awaitable<void> invokeSpawnAwaitable(Ctx& ctx, OpCtl& ctl, Fn& fn) {
    using Ret = std::decay_t<std::invoke_result_t<Fn&, Ctx&, OpCtl&>>;
    if constexpr (std::is_same_v<Ret, asio::awaitable<void>>) {
        co_await fn(ctx, ctl);
    } else {
        (void)co_await fn(ctx, ctl);
    }
}

/// 把 `fn(ctx, ctl)` 的 `asio::awaitable<T>` 结果统一收成 `asio::awaitable<void>`
///
/// 闭包与 `OpCtl` **按值进协程帧**: 记录 ([SpawnRecord]) 在 stop/清理时会被提前
/// 释放, 帧里只存引用就是 use-after-free (ASan 实测)。
template<typename Ctx, typename Fn>
inline asio::awaitable<void> invokeSpawnAwaitableOwned(
    Ctx&                                      ctx,
    OpCtl                                     ctl,
    Fn                                        fn,
    std::shared_ptr<asio::cancellation_signal> signal
) {
    // `signal` 只为了在帧里持一份: 协程的取消槽指向该信号内部, 帧比记录活得久时
    // 记录已释放 → 槽会变成悬垂指针 (ASan 实测在 `cancellation_slot::clear` 报)。
    (void)signal;
    using Ret = std::decay_t<std::invoke_result_t<Fn&, Ctx&, OpCtl&>>;
    if constexpr (std::is_same_v<Ret, asio::awaitable<void>>) {
        co_await fn(ctx, ctl);
    } else {
        (void)co_await fn(ctx, ctl);
    }
}

/// `spawn` 的实现: 任务体是 `asio::awaitable<T>`
///
/// - 协程帧由 asio 管理 (`asio::co_spawn`), 完成处理器上报一次通知;
/// - 取消: 置取消标记 (`OpCtl::isCanceled()`) + 发取消信号, 业务体里正在等的表调用
///   会被 `op_cancel` 撤掉;
/// - 首步同样不在调用方栈里跑: `co_spawn` 会把帧投递到桥的本地执行器, 等主程序下一次
///   driver 推进。
template<typename Ctx, typename Fn>
inline bool spawnAwaitableImpl(Ctx& ctx, Fn&& fn) {
    using SpawnRecord                  = typename std::remove_reference_t<Ctx>::SpawnRecord;
    auto cancelFlag                    = std::make_shared<std::atomic<bool>>(false);
    auto rec                           = std::make_shared<SpawnRecord>();
    rec->cancelFlag                    = cancelFlag;
    std::weak_ptr<SpawnRecord> recWeak = rec;

    PluginxxOperatorNotify  hostNotify{nullptr, nullptr};
    PluginxxOperatorHandle* taskHandle = nullptr;
    if (ctx.iface.tasks && ctx.iface.tasks->register_task) {
        PluginxxOperatorNotify notify{nullptr, nullptr};
        PluginxxString         err{nullptr, 0};
        taskHandle = ctx.iface.tasks->register_task(
            ctx.host,
            [](void* ud, void*) {
                auto* r = static_cast<SpawnRecord*>(ud);
                if (r && r->cancelFlag) {
                    r->cancelFlag->store(true, std::memory_order_release);
                }
                if (r && r->cancelSignal) {
                    r->cancelSignal->emit(asio::cancellation_type::total);
                }
            },
            rec.get(),
            &notify,
            &err
        );
        if (taskHandle) {
            hostNotify = notify;
        } else {
            if (err.data) {
                ctx.log.warn(fmt::format(
                    "spawn: register_task failed: {}",
                    std::string_view{err.data, static_cast<size_t>(err.size)}
                ));
                if (ctx.host) {
                    PluginString::free(ctx.host, &err);
                }
            } else {
                ctx.log.warn("spawn: register_task failed");
            }
            return false;
        }
    } else {
        ctx.log.warn("spawn: host has no pluginxx.tasks iface");
        return false;
    }

    auto closure = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
    if (!rec->ctl) {
        rec->ctl = std::make_shared<OpCtl>(OpCtl{cancelFlag, nullptr, std::string{}, {}});
    }
    auto ctl         = rec->ctl;
    auto cancelSignal = std::make_shared<asio::cancellation_signal>();
    rec->cancelSignal = cancelSignal;
    ctx.spawns_.push_back(rec);

    // asio 协程的每一步都由 `asio::post` 回到桥的本地执行器, 而本地执行器只在主程序
    // 驱动时被 poll: 用一条"受控轮询根"让桥在任务存活期间持续申请驱动 (≤10 ms 一次),
    // 否则第一个协程步就没人推进 (实测会挂住 mount)。根自带的通知器留空 ——
    // 完成上报由下面的完成处理器做。
    auto pumpRoot = std::make_shared<detail::PolledRoot>(PluginxxOperatorNotify{nullptr, nullptr});
    ctx.bridge().addPolledRoot(pumpRoot);
    // 首个驱动请求得自己提: 登记轮询根只让桥“继续”申请请求, 不会自己发起
    ctx.bridge().wake();

    asio::co_spawn(
        ctx.bridge().local_executor(),
        detail::invokeSpawnAwaitableOwned(ctx, *ctl, std::move(*closure), cancelSignal),
        // 取消信号接到协程的取消槽: 主程序取消后正在等的表调用会被 op_cancel 掉
        asio::bind_cancellation_slot(
            cancelSignal->slot(),
            [recWeak, ctl, hostNotify, pumpRoot, &bridge = ctx.bridge()](std::exception_ptr ep) {
                bridge.removePolledRoot(pumpRoot);
                if (hostNotify.done) {
                    hostNotify.done(
                        hostNotify.host_ud,
                        ep ? PLUGINXX_OPERATOR_FAILED : PLUGINXX_OPERATOR_OK,
                        nullptr
                    );
                }
            }
        )
    );
    return true;
}

/// “可取消的业务根”公共接口
///
/// 主程序只持有一个 `void*` 句柄, 取消时回传给接口表的 `cancel`; 两种形态的根
/// (asio provider 根 [ProviderRoot] / kit 协程 Job) 都从这里派生, 取消入口不需要
/// 知道具体是哪种。
struct CancelableProvider {
    virtual void requestCancel() noexcept = 0;
    virtual ~CancelableProvider()           = default;
};

/// 业务协程 provider 根的公共状态
///
/// 用到它的入口: 能力行 ([capability])、图节点 ([graph_node])、主程序侧的功能埋点
/// 实现行 (`providePoint`) —— 它们都是“插件业务协程算出一个值, 交回主程序”。
///
/// 收尾与受控轮询工具一致: 上报权由 [PolledRoot::claimFinish] 认领, 桥停止/关闭
/// 路径可能先一步接管。
struct ProviderRoot : CancelableProvider {
    PluginxxOperatorNotify notify{nullptr, nullptr};
    /// 业务入参等需要活到协程结束的对象的保活位 (插件侧放进去, 根回收时一起释放)
    std::shared_ptr<void> keepAlive;
    /// asio 取消信号: 主程序取消时发一次 (业务协程自己决定是否响应)
    std::shared_ptr<asio::cancellation_signal> cancelSignal
        = std::make_shared<asio::cancellation_signal>();
    /// 受控轮询根 (认领上报权; 存活期间让桥持续申请驱动)
    std::shared_ptr<PolledRoot> root;
    PollOneBridge*             bridge = nullptr;

    /// 请求取消: 只发信号, 不强断协程
    ///
    /// 用 `total` 而不是 `terminal`: `terminal` 会让 asio **立刻销毁协程帧**,
    /// 而业务协程可能正等着一次表调用 (定时器 / 网络); 那次调用稍后完成时还会
    /// 去恢复已经销毁的帧 (实测崩溃在 resumed frame 里).
    /// `total` 只把取消传给正在等的那次操作: 它的取消槽会先 `op_cancel` 掉,
    /// 主程序随后以 CANCELED 完成, 协程在"下一次恢复"时抛 `operation_aborted`
    /// 并正常展开, 帧的生命周期因此是安全的。
    void requestCancel() noexcept override {
        if (cancelSignal) {
            cancelSignal->emit(asio::cancellation_type::total);
        }
    }

    /// 上报终态 (恰好一次; 停止路径可能已认领)
    void finish(int32_t status, std::string_view body) noexcept {
        if (!root || !root->claimFinish()) {
            return;
        }
        if (bridge) {
            bridge->removePolledRoot(root);
        }
        root->notifyHost(status, body);
    }
};

/// 跑一条业务协程, 完成时把值/异常转成 (终态, 正文) 并上报
///
/// `onValue` / `onError` 的返回值都是 `std::pair<int32_t, std::string>`。
template<typename AwaitableT, typename OnValue, typename OnError>
inline asio::awaitable<void> runProviderRoot(
    std::shared_ptr<ProviderRoot> pr,
    AwaitableT                    aw,
    OnValue                       onValue,
    OnError                       onError
) {
    int32_t     status = PLUGINXX_OPERATOR_OK;
    std::string body;
    try {
        auto value = co_await std::move(aw);
        auto pair  = onValue(std::move(value));
        status     = pair.first;
        body       = std::move(pair.second);
    } catch (...) {
        auto pair = onError(std::current_exception());
        status    = pair.first;
        body      = std::move(pair.second);
    }
    pr->finish(status, body);
}

/// 把业务协程排进桥的本地执行器当 provider 根推进
///
/// - `makeAwaitable(pr)` 在根建好之后调用 (业务入参可以指向根拥有的数据);
/// - 首步自己 `wake()`: 本地执行器只在主程序驱动时被 poll;
/// - 返回的根指针就是主程序侧的可取消句柄 ([ProviderRoot::requestCancel])。
template<typename MakeAw, typename OnValue, typename OnError>
inline std::shared_ptr<ProviderRoot> startProviderRoot(
    PollOneBridge&                bridge,
    const PluginxxOperatorNotify& notify,
    MakeAw&&                      makeAw,
    OnValue&&                     onValue,
    OnError&&                     onError
) {
    auto pr    = std::make_shared<ProviderRoot>();
    pr->notify = notify;
    pr->bridge = &bridge;
    pr->root   = std::make_shared<PolledRoot>(notify);
    bridge.addPolledRoot(pr->root);
    // 建协程帧并排进本地执行器 (首步恒异步), 然后自己提一次驱动请求
    auto aw = makeAw(pr);
    bridge.wake();
    asio::co_spawn(
        bridge.local_executor(),
        detail::runProviderRoot<decltype(aw), std::decay_t<OnValue>, std::decay_t<OnError>>(
            pr,
            std::move(aw),
            std::forward<OnValue>(onValue),
            std::forward<OnError>(onError)
        ),
        // 业务协程的取消槽接到根的取消信号上: [ProviderRoot::requestCancel] 因此能
        // 撤销协程里正在等的表调用 (例如计时器 / 模型请求)。
        asio::bind_cancellation_slot(pr->cancelSignal->slot(), asio::detached)
    );
    return pr;
}
} // namespace detail

/// 起一条后台协作任务 (任务体写成 `asio::awaitable<void>`)
///
/// asio 管帧 (`co_spawn` 到桥的本地执行器), 完成时上报一次。
///
/// 入参形如 (Ctx&, OpCtl&) (也可以只收 Ctx&), 闭包按值搬进协程帧。
///
/// - `return` true = 已排入任务表并将被推进; false = 没排上 (主机没有 tasks 表 /
///   register_task 拒绝), 任务体一行都不会执行 — 调用方如果要"完成时必须交回结果",
///   必需检查这个返回值 (否则完成通知永远不会发出)
/// - 需要 `finishLater` 之类自动完成兜底的场景, 用它的 guard 更简单
///   (guard 在闭包被丢弃时自动交 FAILED)
template<typename Ctx, typename Fn>
inline bool spawn(Ctx& ctx, Fn&& fn) {
    return detail::spawnAwaitableImpl(ctx, std::forward<Fn>(fn));
}

/* ==================== 插件实例上下文基类 (通用部分) ==================== */

/// 插件实例上下文基类 (通用部分; header-only)
///
/// - 持有主程序句柄、通用接口表聚合、实例级 [Logger] 与 [CancelRegistry];
/// - 提供与主程序业务无关的常用操作: config / workDir / argsJson / configPath /
///   language / setLanguage / jsonEscape / jsonGetString /
///   主程序堆字符串 (strdup / createString / createPluginString) / 后台任务 spawn;
/// - 协程驱动桥 (见 [bridge]) 让插件协程与主程序协程在同一 IO 执行序列中交错推进;
/// - 业务能力由主程序侧派生基类补齐: 派生类既可在 `IfacesT` 中带业务表, 也可覆写
///   [onHostReady] 挂钩业务初始化 (由主程序派生类实现)。
template<typename IfacesT>
class PluginBaseT {
public:

    const PluginxxHost* host = nullptr;
    IfacesT             iface;
    Logger              log;
    CancelRegistry cancelRegistry; ///< 框架级事件驱动取消注册表 (每个实例独立一份)

    PluginBaseT() = default;

    virtual ~PluginBaseT() {
        if (lifeToken_) {
            lifeToken_->store(false, std::memory_order_release);
        }
        cancelRegistry.cancelAll();
        stopSpawns();
        // 桥 (协程驱动) 必须最后销毁: 它会取消排队请求并终结仍在排队的根, 而根的
        // 收束可能仍需访问上面的实例状态。
        stopBridge();
    }

    std::shared_ptr<std::atomic<bool>> lifeToken() const {
        return lifeToken_;
    }

    /// 本实例的协程驱动桥 (延迟创建; 每实例一份, 无进程级可变状态)。
    /// - 线程: 只应在主程序 IO 线程调用 (与实例状态同一串行上下文)
    detail::PollOneBridge& bridge() const {
        if (!bridge_) {
            bridge_ = std::make_unique<detail::PollOneBridge>(
                host,
                iface.scheduler,
                iface.cancel
            );
        }
        return *bridge_;
    }

    /// 桥 (协程驱动) 必须最后销毁: 它会取消排队请求并终结仍在排队的根, 而根的
    /// 收束可能仍需访问上面的实例状态。
    void stopBridge() noexcept {
        if (bridge_) {
            bridge_->stop();
        }
    }

    /// 绑定主程序句柄与接口表 (同步; 只做字段赋值)
    ///
    /// 与业务初始化分开的原因: create 事务要 `co_await` 业务初始化 (例如订阅广播),
    /// 而 `spawn` 本身就依赖 `host` / `iface`, 所以绑定必须先同步完成。
    void attachHost(const PluginxxHost* h) {
        host         = h;
        iface        = IfacesT::query(h);
        log.host     = h;
        log.logIface = iface.log;
        log.logFn    = (iface.log && iface.log->log) ? iface.log->log : nullptr;
    }

    /// 主程序配置 JSON (**协程形态**; 失败给 `{}`)
    asio::awaitable<std::string> configAsync() const {
        if (!host || !iface.config || !iface.config->get_config) {
            co_return std::string{"{}"};
        }
        auto r = co_await asyncTableOp(*this, co_await asio::this_coro::executor, [this](PluginxxOperatorCallback cb,
                                                                 void*           ud,
                                                                 PluginxxString* err) {
            return iface.config->get_config(host, cb, ud, err);
        });
        co_return (r.ok() && !r.text().empty()) ? std::string{r.text()} : std::string{"{}"};
    }

    /// 本插件配置参数 JSON (**协程形态**; 失败给 `{}`)
    asio::awaitable<std::string> argsJsonAsync() const {
        if (!host || !iface.config || !iface.config->get_plugin_args) {
            co_return std::string{"{}"};
        }
        auto r = co_await asyncTableOp(*this, co_await asio::this_coro::executor, [this](PluginxxOperatorCallback cb,
                                                                 void*           ud,
                                                                 PluginxxString* err) {
            return iface.config->get_plugin_args(host, cb, ud, err);
        });
        co_return r.ok() ? std::string{r.text()} : std::string{"{}"};
    }

    /// 本插件配置文件路径 (**协程形态**; 未配置给空串)
    asio::awaitable<std::string> configPathAsync() const {
        if (!host || !iface.config || !iface.config->get_plugin_config_path) {
            co_return std::string{};
        }
        auto r = co_await asyncTableOp(*this, co_await asio::this_coro::executor, [this](PluginxxOperatorCallback cb,
                                                                 void*           ud,
                                                                 PluginxxString* err) {
            return iface.config->get_plugin_config_path(host, cb, ud, err);
        });
        co_return r.ok() ? std::string{r.text()} : std::string{};
    }

    /// 当前语言 (**协程形态**; 取不到给 `en`; 高频值建议缓存到实例上)
    asio::awaitable<std::string> languageAsync() const {
        if (!host || !iface.config || !iface.config->get_language) {
            co_return std::string{"en"};
        }
        auto r = co_await asyncTableOp(*this, co_await asio::this_coro::executor, [this](PluginxxOperatorCallback cb,
                                                                 void*           ud,
                                                                 PluginxxString* err) {
            return iface.config->get_language(host, cb, ud, err);
        });
        co_return (r.ok() && !r.text().empty()) ? std::string{r.text()} : std::string{"en"};
    }

    /// 切换语言 (**协程形态**; 返回是否受理且成功)
    asio::awaitable<bool> setLanguageAsync(std::string lang) const {
        if (!host || !iface.config || !iface.config->set_language) {
            co_return false;
        }
        auto langSv = PluginStringView::from(lang);
        auto r      = co_await asyncTableOp(
            *this,
            co_await asio::this_coro::executor,
            [this, &langSv](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.config->set_language(host, &langSv, cb, ud, err);
            }
        );
        co_return r.ok();
    }

    char* strdup(PluginxxStringView sv) const {
        return PluginString::strdup(host, &sv);
    }

    char* strdup(std::string_view sv) const {
        return PluginString::strdup(host, sv);
    }

    char* strdup(const char* s) const {
        if (!s) {
            return nullptr;
        }
        auto sv = PluginStringView::fromCstr(s);
        return PluginString::strdup(host, &sv);
    }

    PluginxxString createString(PluginxxStringView sv) const {
        return PluginString::from(host, &sv);
    }

    PluginxxString createString(std::string_view sv) const {
        return PluginString::from(host, sv);
    }

    PluginString createPluginString(PluginxxStringView sv) const {
        return PluginString::create(host, sv);
    }

    PluginString createPluginString(std::string_view sv) const {
        return PluginString::create(host, sv);
    }

    /// 字符串 → JSON 字符串字面量 (经主程序 pluginxx.json 接口表; 含引号包裹与转义)
    std::string jsonEscape(PluginxxStringView s) const {
        if (!host || !iface.json || !iface.json->json_escape || (!s.data && s.size == 0)) {
            return "\"\"";
        }
        PluginxxString esc{nullptr, 0};
        auto           sSv = PluginStringView::from(s.data, s.size);
        iface.json->json_escape(host, &sSv, &esc);
        if (!esc.data) {
            return "\"\"";
        }
        std::string out(esc.data, static_cast<size_t>(esc.size));
        PluginString::free(host, &esc);
        return out;
    }

    std::string jsonEscape(std::string_view s) const {
        return jsonEscape(PluginStringView::from(s.data(), s.size()));
    }

    std::string jsonEscape(const char* s) const {
        if (!s) {
            return "\"\"";
        }
        return jsonEscape(std::string_view{s});
    }

    /// 从 JSON 中提取 key 的字符串值 (经主程序 json 接口表; 不存在返回空)
    std::string jsonGetString(std::string_view json, std::string_view key) const {
        if (!host || !iface.json || !iface.json->json_get_string) {
            return {};
        }
        auto           jsonSv = PluginStringView::from(json.data(), json.size());
        auto           keySv  = PluginStringView::from(key.data(), key.size());
        PluginxxString out{nullptr, 0};
        iface.json->json_get_string(host, &jsonSv, &keySv, &out);
        if (!out.data) {
            return {};
        }
        std::string res(out.data, static_cast<size_t>(out.size));
        PluginString::free(host, &out);
        return res;
    }

    /// 注册类 spec 的字符串存储: 用 `std::deque` 而不是 `vector` ——
    /// `push_back` 不会让已存字符串搬位置, 因此 spec 里的 `string_view` 在整个
    /// 实例生命周期内一直有效 (注册是"提交后由主程序异步读取", 不能靠"调用期间
    /// 不扩容"来保证)。
    std::deque<std::string> storage_;

    std::vector<std::unique_ptr<void, void (*)(void*)>> shims_;

    template<typename T>
    T* storeShim(std::unique_ptr<T> shim) {
        T* raw = shim.get();
        shims_.emplace_back(shim.release(), [](void* ptr) {
            delete static_cast<T*>(ptr);
        });
        return raw;
    }

    struct SpawnRecord {
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        /// 后台任务的 OpCtl 由记录持有：任务协程以引用接收它，必须在挂起后仍然
        /// 有效（放在协程栈上会悬垂）。
        std::shared_ptr<OpCtl> ctl;
        std::shared_ptr<asio::cancellation_signal> cancelSignal;
    };

    std::vector<std::shared_ptr<SpawnRecord>> spawns_;

    void stopSpawns() {
        for (auto& rec : spawns_) {
            if (!rec) {
                continue;
            }
            if (rec->cancelSignal) {
                rec->cancelSignal->emit(asio::cancellation_type::total);
            }
        }
        spawns_.clear();
    }

    /// 启动后台协作任务 (完成协议见 [detail::spawnAwaitableImpl])
    /// - 任务体写成 `asio::awaitable<void>`: asio 管帧, `co_spawn` 到桥的本地执行器
    /// - 任务协程以本实例引用接收 OpCtl; 主程序取消/实例析构时由 [stopSpawns] 收束
    /// - `return` false = 没排上 (主机没有 tasks 表 / register_task 拒绝), 任务体不会
    ///   执行; "完成时必须交回通知"的调用方必须检查 (否则对方永远等不到完成)
    template<typename Self, typename Fn>
    bool spawn(this Self& self, Fn&& fn) {
        return detail::spawnAwaitableImpl(self, std::forward<Fn>(fn));
    }

    /// 业务初始化 (**协程**: create 事务在 [attachHost] 之后 await 它)
    ///
    /// 需要调表的初始化 (订阅广播、读配置…) 写在这里: 插件执行线程上不能同步等表,
    /// 协程形态是唯一可用写法。派生类覆写时必须保持"不抛异常"的约定 (抛了由
    /// create 事务转成失败通知)。
    ///
    /// **对外可见是为了让 create 事务能调它** (不是给插件业务代码调的)。
    virtual asio::awaitable<void> onHostReady() { co_return; }

private:

    std::shared_ptr<std::atomic<bool>> lifeToken_ = std::make_shared<std::atomic<bool>>(true);
    /// 协程驱动桥 (延迟创建; 见 [bridge])。声明在最后: 析构顺序需要它在其它成员
    /// 之后销毁 (它在 stop 时会终结仍在排队的根)。
    mutable std::unique_ptr<detail::PollOneBridge> bridge_;
};

/* ==================== 锚定原语 awaiter 族 ==================== */

namespace detail {

enum class AwaiterState : uint32_t {
    INIT      = 0,
    CALLING   = 1,
    SUSPENDED = 2,
    COMPLETED = 3
};

/// 取一个主程序堆字符串出参并把内存还回去 (空出参得到空串)
inline std::string takeHostError(const PluginxxHost* host, PluginxxString& s) {
    std::string out;
    if (s.data) {
        out.assign(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
    }
    return out;
}

/* ---- asio 形态的 offload (给插件自有 io_context 上的 `asio::awaitable` 用) ---- */

/// asio 形态 offload 的结果
///
/// 状态、原因与工作体返回值都放在这个结构里, 完成处理器里**不抛异常** (它可能在
/// 任意线程上跑); 取值/抛异常的规则与 [asyncOffload] 一致, 由调用方那一侧判断。
template<typename ResultT>
struct OffloadOutcome {
    /// `PLUGINXX_OPERATOR_*`
    int32_t status = PLUGINXX_OPERATOR_OK;
    /// 主程序给的失败/取消原因
    std::string error;
    /// 未受理的原因 (非空表示压根没开始)
    std::string startError;
    /// 工作体抛出的异常
    std::exception_ptr workError;
    /// 工作体返回值 (void 时用 monostate 占位)
    std::conditional_t<std::is_void_v<ResultT>, std::monostate, std::optional<ResultT>> value;
};

/// asio 异步操作形态的 offload 状态 (恢复经 `asio::post`, 不经协程驱动桥)
template<typename Handler, typename Executor, typename ResultT, typename WorkFn>
struct AsyncOffloadState {
    Handler                 handler;
    Executor                ex;
    WorkFn                  work;
    OffloadOutcome<ResultT> outcome;
    std::atomic<bool>       fired{false};

    AsyncOffloadState(Handler inHandler, Executor inEx, WorkFn inWork) :
        handler(std::move(inHandler)),
        ex(std::move(inEx)),
        work(std::move(inWork)) {}
};

/// 把结果投到执行器并调用完成处理器 (恰好一次)
template<typename State>
inline void finishAsyncOffload(const std::shared_ptr<State>& st) {
    if (st->fired.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    asio::post(
        st->ex,
        [st]() mutable {
            auto handler = std::move(st->handler);
            handler(std::move(st->outcome));
        }
    );
}

/// asio 异步操作形态的 offload 发起器: 调用处即提交, 结果经 `asio::post` 回到执行器
template<typename Executor, typename ResultT, typename WorkFn>
struct AsyncOffloadInitiation {
    const PluginxxHost*           host  = nullptr;
    const PluginxxSchedulerIface* sched = nullptr;
    Executor                      ex;
    WorkFn                        work;

    template<typename Handler>
    void operator()(Handler&& handler) {
        using H     = std::decay_t<Handler>;
        using State = AsyncOffloadState<H, Executor, ResultT, WorkFn>;
        auto st     = std::make_shared<State>(H(std::forward<Handler>(handler)), ex, std::move(work));
        if (!sched || !sched->offload) {
            st->outcome.status     = PLUGINXX_OPERATOR_FAILED;
            st->outcome.startError = "scheduler offload is unavailable";
            finishAsyncOffload<State>(st);
            return;
        }
        // 主程序给 work 与 done 的是同一个 user_data, 且 done 总在 work 之后:
        // 堆上只留一份持有者, 由 done 回调释放 (work 只借用)
        auto* holder = new std::shared_ptr<State>(st);
        PluginxxString err{nullptr, 0};
        auto*          op = sched->offload(
            host,
            [](void* ud, const PluginxxCancelToken* token, PluginxxString* error_out) -> void* {
                auto* hp = static_cast<std::shared_ptr<State>*>(ud);
                auto  s  = *hp;
                try {
                    if constexpr (std::is_void_v<ResultT>) {
                        s->work(token);
                    } else {
                        s->outcome.value = s->work(token);
                    }
                } catch (...) {
                    s->outcome.workError = std::current_exception();
                }
                if (error_out) {
                    *error_out = PluginxxString{nullptr, 0};
                }
                return nullptr;
            },
            [](void* ud, int32_t status, void*, const PluginxxStringView* errView) {
                auto* hp = static_cast<std::shared_ptr<State>*>(ud);
                auto  s  = std::move(*hp);
                delete hp;
                s->outcome.status = status;
                s->outcome.error  = PluginStringView::str(errView);
                finishAsyncOffload<State>(s);
            },
            holder,
            &err
        );
        if (!op) {
            delete holder;
            st->outcome.status     = PLUGINXX_OPERATOR_FAILED;
            st->outcome.startError = takeHostError(host, err);
            if (st->outcome.startError.empty()) {
                st->outcome.startError = "plugin offload was not accepted";
            }
            finishAsyncOffload<State>(st);
        }
    }
};

/// asio 异步操作对象 (协议同 [AsyncTableOp]: 只认完成签名探针与完成处理器两种调用)
template<typename Executor, typename ResultT, typename WorkFn>
class AsyncOffload {
public:

    AsyncOffload(
        const PluginxxHost*           inHost,
        const PluginxxSchedulerIface* inSched,
        Executor                      inEx,
        WorkFn                        inWork
    ) :
        host_(inHost),
        sched_(inSched),
        ex_(std::move(inEx)),
        work_(std::move(inWork)) {}

    template<typename Token>
        requires(
            std::is_same_v<std::decay_t<Token>, asio::detail::completion_signature_probe>
            || std::is_same_v<std::decay_t<Token>, asio::detail::async_operation_probe>
            || std::is_invocable_v<std::decay_t<Token>&, OffloadOutcome<ResultT>>
        )
    auto operator()(Token&& token) {
        using TokenT = std::decay_t<Token>;
        if constexpr (std::is_same_v<TokenT, asio::detail::completion_signature_probe>) {
            return asio::detail::completion_signature_probe_result<void(OffloadOutcome<ResultT>)>{};
        } else if constexpr (std::is_same_v<TokenT, asio::detail::async_operation_probe>) {
            return asio::detail::async_operation_probe_result{};
        } else {
            auto initiation = AsyncOffloadInitiation<Executor, ResultT, WorkFn>{
                host_,
                sched_,
                ex_,
                std::move(work_)
            };
            initiation(std::forward<Token>(token));
        }
    }

private:

    const PluginxxHost*           host_  = nullptr;
    const PluginxxSchedulerIface* sched_ = nullptr;
    Executor                      ex_;
    WorkFn                        work_;
};

/// 一次表调用的结果 (接口表成员统一走异步操作契约)
///
/// 两种"没拿到结果"要分开看:
/// - `startError` 非空 = 主程序**没有受理**这次调用 (参数错 / 作用域失效 /
///   实例正在关闭), 此时没有完成回调, `status` 无意义;
/// - 已受理但 `status != PLUGINXX_OPERATOR_OK` = 执行期失败或取消, 正文是原因。
struct TableOpResult {
    int32_t     status = PLUGINXX_OPERATOR_FAILED;
    std::string payload;
    std::string startError;

    /// 是否已受理 (false = 没有结果正文, 原因看 [startError])
    bool accepted() const noexcept {
        return startError.empty();
    }
    /// 已受理且执行成功
    bool ok() const noexcept {
        return accepted() && status == PLUGINXX_OPERATOR_OK;
    }
    /// 已受理但被取消
    bool canceled() const noexcept {
        return accepted() && status == PLUGINXX_OPERATOR_CANCELED;
    }
    /// 结果正文 (成功时是结果; 失败时是错误文本)
    std::string_view text() const noexcept {
        return payload;
    }
    /// 按 `1`/`0` 文本读标记 (旧接口用返回码表达"是/否"的成员)
    ///
    /// 空正文、`0`、`false`、`null` 都算 false; 其余非空文本算 true。
    bool flag() const noexcept {
        const auto v = trimmed();
        return !v.empty() && v != "0" && v != "false" && v != "null";
    }
    /// 按十进制文本读整数 (读不到用 `fallback`)
    int64_t integer(int64_t fallback = 0) const noexcept {
        long long v = 0;
        if (!parseWholeInteger(trimmed(), v)) {
            return fallback;
        }
        return static_cast<int64_t>(v);
    }

private:

    std::string_view trimmed() const noexcept {
        std::string_view v{payload};
        while (!v.empty() && isArgSpace(v.front())) {
            v.remove_prefix(1);
        }
        while (!v.empty() && isArgSpace(v.back())) {
            v.remove_suffix(1);
        }
        return v;
    }
};

/// asio 异步操作形态的表调用状态 (不经协程驱动桥, 恢复经 `asio::post`)
/// 完成经 `asio::post` 投回调用方给的那个执行器)
///
/// 为什么需要它: 插件自有 io 线程 (自建 `io_context`) 上的协程是 `asio::awaitable`,
/// 那种协程只认 `awaitable` 与 asio 异步操作两类可等待对象, 自定义 awaiter 写不进去。
/// 所以这里把同一次表调用包成标准 asio 异步操作, 由调用方指定"在哪个执行器上恢复"。
template<typename Handler, typename Executor>
struct AsyncTableOpState {
    Handler           handler;
    Executor          ex;
    std::atomic<bool> fired{false};
    /// 本次表调用在主程序侧的操作句柄 (槽里取消时用它调 `op_cancel`)
    std::atomic<PluginxxOperatorHandle*> opHandle{nullptr};
    /// 实例存活标记 (**弱引用**): 实例已析构时不再投递/恢复协程
    ///
    /// 为什么要它: 完成回调可能比实例活得久 (主程序计时器 / 网络 / 卸载竞态)。
    /// 实例析构后执行器 (`io_context`) 和协程帧都已释放, 再投递就是向已释放的
    /// 执行器上排队, 恢复的是一个已释放的帧 (实测直接崩溃在协程恢复函数里)。
    std::weak_ptr<std::atomic<bool>> life;

    AsyncTableOpState(Handler inHandler, Executor inEx) :
        handler(std::move(inHandler)),
        ex(std::move(inEx)) {}
};

/// 实例是否还活着 (没有存活标记时按"活"处理: 调用方没给上下文)
inline bool tableOpOwnerAlive(const std::weak_ptr<std::atomic<bool>>& life) noexcept {
    const auto token = life.lock();
    return !token || token->load(std::memory_order_acquire);
}

/// 把结果投到执行器并调用完成处理器 (恰好一次)
///
/// 完成回调可能来自任何线程 (主程序 IO / 插件执行线程, 甚至 `start` 内部就地完成),
/// 因此这里**只 post 不直接调**: 处理器恢复的是调用方那个执行器上的协程。
template<typename Handler, typename Executor>
inline void finishAsyncTableOp(
    const std::shared_ptr<AsyncTableOpState<Handler, Executor>>& st,
    TableOpResult                                                result
) {
    if (st->fired.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // 实例已经没了: 不投递 (执行器与协程帧都已释放; 帧由 asio 自己回收)
    if (!tableOpOwnerAlive(st->life)) {
        return;
    }
    asio::post(
        st->ex,
        [st, result = std::move(result)]() mutable {
            // 排队期间实例可能被析构: 投递到本地后才检查, 仍是安全点
            if (!tableOpOwnerAlive(st->life)) {
                return;
            }
            auto handler = std::move(st->handler);
            handler(std::move(result));
        }
    );
}

/// asio 异步操作形态的表调用发起器: 调用处即提交, 结果经 `asio::post` 回到执行器
template<typename Executor, typename StartFn>
struct AsyncTableOpInitiation {
    const PluginxxHost*        host        = nullptr;
    const PluginxxCancelIface* cancelIface = nullptr;
    Executor                   ex;
    StartFn                    start;
    /// 调用方协程的取消槽 (未连接 = 不接取消)
    asio::cancellation_slot slot;
    /// 实例存活标记 (见 [AsyncTableOpState::life])
    std::weak_ptr<std::atomic<bool>> life;

    template<typename Handler>
    void operator()(Handler&& handler) {
        using H     = std::decay_t<Handler>;
        using State = AsyncTableOpState<H, Executor>;
        auto st     = std::make_shared<State>(H(std::forward<Handler>(handler)), ex);
        st->life    = life;
        auto* holder = new std::shared_ptr<State>(st);
        PluginxxString err{nullptr, 0};
        // 完成回调: 主程序受理后至少调用一次 (未受理则不会调用, 由下面的
        // `start` 返回值判定), 因此这里直接接管 holder 的所有权
        auto* op = start(
            [](void* ud, int32_t status, const PluginxxStringView* payload) {
                auto* hp = static_cast<std::shared_ptr<State>*>(ud);
                auto  s  = std::move(*hp);
                delete hp;
                // 操作已结束: 之后的取消不能再拿这个句柄去调 op_cancel
                s->opHandle.store(nullptr, std::memory_order_release);
                TableOpResult r;
                r.status = status;
                if (payload && payload->data && payload->size > 0) {
                    r.payload.assign(payload->data, static_cast<size_t>(payload->size));
                }
                finishAsyncTableOp<H, Executor>(s, std::move(r));
            },
            holder,
            &err
        );
        if (op == nullptr) {
            delete holder;
            TableOpResult r;
            r.startError = takeHostError(host, err);
            if (r.startError.empty()) {
                r.startError = "plugin table call was not accepted";
            }
            finishAsyncTableOp<H, Executor>(st, std::move(r));
            return;
        }
        // 完成回调可能在 start() 内部就地跑过 (主程序内联完成, 例如网络表的
        // `net.info` / `ws.send` 一类立即完成的 op): 那时回调已经把 opHandle 清空并
        // 置位 fired, 这里不能再把过期句柄写回去 —— 操作完成很久之后句柄可能已被
        // 主程序回收, 取消槽再拿它调 `op_cancel` 就是 use-after-free。
        if (!st->fired.load(std::memory_order_acquire)) {
            st->opHandle.store(op, std::memory_order_release);
        }
        // 协程被取消 (主程序关闭 / 业务主动取消) 时把同一次调用在主程序侧撤掉,
        // 否则插件会一直等到计时器/IO 自己结束。
        if (slot.is_connected() && cancelIface != nullptr && cancelIface->op_cancel != nullptr) {
            slot.assign([st, cancelIface = cancelIface](asio::cancellation_type) {
                // 已完成: 句柄不再有效 (见上面那段)
                if (st->fired.load(std::memory_order_acquire)) {
                    return;
                }
                if (auto* h = st->opHandle.exchange(nullptr, std::memory_order_acq_rel)) {
                    try {
                        cancelIface->op_cancel(h);
                    } catch (...) {
                    }
                }
            });
        }
    }
};

/// asio 异步操作对象 (实现了 asio 的异步操作协议: `op(token)`)
///
/// 协议只覆盖两件事, 因此不去碰 `asio::async_initiate` (它的显式模板参数顺序在不同
/// Boost 版本里不一致, 传签名和传令牌谁在前都试过, 会难读到无法维护):
/// - `op(completion_signature_probe)` → 交回完成签名 `void(TableOpResult)` (asio 用它
///   推断 `co_await` 的结果类型);
/// - `op(handler)` → 直接发起, 完成处理器就是传进来的那个 (asio 的 awaitable 走的
///   就是这条)。
///
/// 其它完成令牌 (例如 `asio::detached`) 不支持: 需要的话把它包进协程即可。
template<typename Executor, typename StartFn>
class AsyncTableOp {
public:

    AsyncTableOp(
        const PluginxxHost*        inHost,
        const PluginxxCancelIface* inCancel,
        Executor                   inEx,
        StartFn                    inStart,
        asio::cancellation_slot    inSlot = {},
        std::weak_ptr<std::atomic<bool>> inLife = {}
    ) :
        host_(inHost),
        cancelIface_(inCancel),
        ex_(std::move(inEx)),
        start_(std::move(inStart)),
        slot_(inSlot),
        life_(std::move(inLife)) {}

    template<typename Token>
        requires(
            std::is_same_v<std::decay_t<Token>, asio::detail::completion_signature_probe>
            || std::is_same_v<std::decay_t<Token>, asio::detail::async_operation_probe>
            || std::is_invocable_v<std::decay_t<Token>&, TableOpResult>
        )
    auto operator()(Token&& token) {
        using TokenT = std::decay_t<Token>;
        if constexpr (std::is_same_v<TokenT, asio::detail::completion_signature_probe>) {
            return asio::detail::completion_signature_probe_result<void(TableOpResult)>{};
        } else if constexpr (std::is_same_v<TokenT, asio::detail::async_operation_probe>) {
            return asio::detail::async_operation_probe_result{};
        } else {
            auto initiation = AsyncTableOpInitiation<Executor, StartFn>{
                host_,
                cancelIface_,
                ex_,
                start_,
                slot_,
                life_
            };
            initiation(std::forward<Token>(token));
        }
    }

private:

    const PluginxxHost*        host_        = nullptr;
    const PluginxxCancelIface* cancelIface_ = nullptr;
    Executor                   ex_;
    StartFn                    start_;
    asio::cancellation_slot    slot_;
    /// 实例存活标记 (见 [AsyncTableOpState::life])
    std::weak_ptr<std::atomic<bool>> life_;
};

} // namespace detail

namespace detail {

/// 取当前协程的执行器与取消槽后提交一次表调用 (asio 形态的公共入口)
///
/// 取消槽连上时, 协程被取消会把同一次调用在主程序侧 `op_cancel` 掉
/// (见 [AsyncTableOpInitiation]); 未连接则与不带槽的重载一致。
template<typename Ctx, typename StartFn>
inline asio::awaitable<TableOpResult> tableOpAwaitAsyncSlot(const Ctx& ctx, StartFn start) {
    auto ex = co_await asio::this_coro::executor;
    auto cs = co_await asio::this_coro::cancellation_state;
    co_return co_await asyncTableOp(ctx, ex, cs.slot(), std::move(start));
}

} // namespace detail

/// 计时器的 **asio 形态** (插件里唯一的计时入口)
///
/// 完成经 `asio::post` 投回当前协程的执行器。走的是主程序 `scheduler.sleep`
/// (插件本地没有自己会流动的 reactor, 自建 `steady_timer` 不会自行到期)。
///
/// - 失败/被拒抛 `std::runtime_error`, 取消抛 [CanceledException];
/// - `ms <= 0` 直接返回, 不提交计时。
template<typename Ctx>
inline asio::awaitable<void> sleepAsync(Ctx& ctx, int64_t ms) {
    if (ms <= 0) {
        co_return;
    }
    if (!ctx.iface.scheduler || !ctx.iface.scheduler->sleep) {
        throw std::runtime_error("scheduler sleep is unavailable");
    }
    auto r = co_await detail::tableOpAwaitAsyncSlot(
        ctx,
        [&ctx, ms](PluginxxOperatorCallback cb, void* ud, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            return ctx.iface.scheduler->sleep(ctx.host, ms, cb, ud, err);
        }
    );
    if (!r.accepted()) {
        throw std::runtime_error(
            r.startError.empty() ? "plugin sleep was not accepted" : r.startError
        );
    }
    if (r.status == PLUGINXX_OPERATOR_CANCELED) {
        throw CanceledException(r.text().empty() ? "sleep canceled" : std::string{r.text()});
    }
    if (!r.ok()) {
        throw std::runtime_error(r.text().empty() ? "sleep failed" : std::string{r.text()});
    }
    co_return;
}

/// 表调用的结果类型 (`pluginxx::detail::TableOpResult` 的对外别名)
using TableOpResult = detail::TableOpResult;

/// 调用接口表成员, 包成 **asio 异步操作** (可用 `co_await` 于 `asio::awaitable`)
///
/// 完成经 `asio::post(ex, ...)` 投到调用方给的那个执行器, 与 kit 的驱动桥无关。
/// 典型场景: 插件自带 `io_context` (网络/协议循环), 业务代码是 asio 协程。
///
/// - `ex`: 恢复协程用的执行器 (通常是那个 io_context 的 executor)。完成回调可能
///   来自任何线程, 因此**必须**给对执行器, 否则协程会跑错线程;
/// - 提交发生在调用处 (不是等 `co_await` 才提交);
/// - 完成签名是 `void(pluginxx::TableOpResult)`: 业务失败不抛异常, 看 `ok()` /
///   `text()` / `accepted()`;
/// - 同一对象也能给回调形态用 (任何 asio 完成标记: `asio::detached` / lambda);
/// - 不接协程取消: 需要取消时在 `start` 里自行保存句柄后调 `cancel.op_cancel`。
///
/// ```cpp
/// auto r = co_await pluginxx::asyncTableOp(
///     ctx,
///     ex,
///     [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
///         return iface->get_messages(host, scope, &sid, cb, ud, err);
///     }
/// );
/// ```
template<typename Ctx, typename Executor, typename StartFn>
inline auto asyncTableOp(const Ctx& ctx, Executor ex, StartFn&& start) {
    return detail::AsyncTableOp<Executor, std::decay_t<StartFn>>{
        ctx.host,
        ctx.iface.cancel,
        std::move(ex),
        std::forward<StartFn>(start),
        {},
        ctx.lifeToken()
    };
}

/// 表调用的 asio 异步操作 (带取消槽)
///
/// 与上一个重载完全相同, 只是多给一个取消槽: 槽被触发时把同一次调用在主程序
/// 侧的操作 `op_cancel` 掉, 插件协程因此不会干等计时器/IO 自己结束。
/// 典型的传入值 = `co_await asio::this_coro::cancellation_state` 的 `slot()`。
template<typename Ctx, typename Executor, typename StartFn>
inline auto
    asyncTableOp(const Ctx& ctx, Executor ex, asio::cancellation_slot slot, StartFn&& start) {
    return detail::AsyncTableOp<Executor, std::decay_t<StartFn>>{
        ctx.host,
        ctx.iface.cancel,
        std::move(ex),
        std::forward<StartFn>(start),
        slot,
        ctx.lifeToken()
    };
}


/// 把阻塞工作交给主程序工作池, 包成 **asio 形态** (可用 `co_await` 于 `asio::awaitable`)
///
/// 完成经 `asio::post(ex, ...)` 投到调用方给的那个执行器, 因此插件自有 `io_context`
/// 上的业务协程也能用。
///
/// - `work` 形如 `ResultType(const PluginxxCancelToken*)`, 在工作池线程上跑;
///   取消时主程序会把令牌置为已请求 (协作式, 不会强行打断);
/// - 提交发生在调用处 (不是等 `co_await` 才提交);
/// - 结果是一个 [OffloadOutcome] 结构 (**完成处理器里不抛异常**): 未受理、取消、
///   失败与工作体异常都在其中, 由调用方决定怎么处理 (agent 侧的 `offloadAsync`
///   已把这些换成异常);
/// - 不接协程取消: 需要取消时自己保存令牌/句柄再走 `cancel.op_cancel`。
///
/// ```cpp
/// auto outcome = co_await pluginxx::asyncOffload(ctx, ex, [&](const PluginxxCancelToken*) {
///     return doBlockingWork();   // 目录遍历 / 全文件扫描这类 CPU 工作
/// });
/// ```
template<typename Ctx, typename Executor, typename WorkFn>
inline auto asyncOffload(const Ctx& ctx, Executor ex, WorkFn&& work) {
    using ResultT = std::decay_t<std::invoke_result_t<WorkFn&, const PluginxxCancelToken*>>;
    return detail::AsyncOffload<Executor, ResultT, std::decay_t<WorkFn>>{
        ctx.host,
        ctx.iface.scheduler,
        std::move(ex),
        std::forward<WorkFn>(work)
    };
}



/// 调用接口表成员并把结果交给回调 (**回调形态**, 任意线程)
///
/// `done` 在主程序 IO 线程被调用恰好一次 (含"未受理"的情况, 此时
/// `result.accepted() == false`)。回调里不要长时间阻塞: 那是主程序 IO 线程。
template<typename Ctx, typename StartFn, typename DoneFn>
inline void tableOpCallback(const Ctx& ctx, StartFn&& start, DoneFn&& done) {
    using Done = std::decay_t<DoneFn>;
    /// 回调上下文: 第一字段只是一块占位 (让整个结构至少 8 字节)
    /// - 主程序不读 `user_data` 的内容 (只按原样透传), 但会对它按指针读 8 字节,
    ///   所以不能直接把很小的函子当 `user_data` 用 (空函子只占 1 字节 → 越界读)
    struct Holder {
        uint64_t magic = 0;
        Done     done;
    };
    auto* work = new Holder{0, std::forward<DoneFn>(done)};
    PluginxxString err{nullptr, 0};
    auto handle = start(
        [](void* ud, int32_t status, const PluginxxStringView* payload) {
            std::unique_ptr<Holder> fn(static_cast<Holder*>(ud));
            detail::TableOpResult r;
            r.status = status;
            if (payload && payload->data && payload->size > 0) {
                r.payload.assign(payload->data, static_cast<size_t>(payload->size));
            }
            (fn->done)(r);
        },
        work,
        &err
    );
    if (!handle) {
        std::unique_ptr<Holder> fn(work);
        detail::TableOpResult  r;
        r.startError = detail::takeHostError(ctx.host, err);
        if (r.startError.empty()) {
            r.startError = "plugin table call was not accepted";
        }
        (fn->done)(std::move(r));
    }
}

namespace detail {

/// 按可调用性选择能力业务签名：fn(ctx, caller, method, args) /
/// fn(ctx, method, args) / fn(method, args)。
/// 返回类型原样转发：同步能力返回字符串，异步能力返回 `asio::awaitable<T>`
/// （与 [invokeHook] 相同的严格分发策略）。
template<typename CapFn, typename Ctx>
inline decltype(auto) invokeCap(
    CapFn&              fn,
    Ctx&                ctx,
    const PluginxxHost* caller,
    std::string_view    method,
    std::string_view    args
) {
    if constexpr (std::is_invocable_v<
                      CapFn,
                      Ctx&,
                      const PluginxxHost*,
                      std::string_view,
                      std::string_view>) {
        return fn(ctx, caller, method, args);
    } else if constexpr (std::is_invocable_v<CapFn, Ctx&, std::string_view, std::string_view>) {
        return fn(ctx, method, args);
    } else {
        return fn(method, args);
    }
}

} // namespace detail

/// 能力登记的选项 (不填时 = 声明自己的名字 + 单行覆盖 + 只开放 impl 阶段)
struct CapabilityOptions {
    /// 一个 id 上有几个实现: `PLUGINXX_CAPABILITY_IMPLS_ONE|MANY` (声明自己的名字时必填)
    int32_t impls = PLUGINXX_CAPABILITY_IMPLS_ONE;
    /// 实现链怎么合并: `PLUGINXX_CAPABILITY_MERGE_NONE|FIRST|VETO`
    int32_t merge = PLUGINXX_CAPABILITY_MERGE_NONE;
    /// 声明开放的阶段 (位或): `PLUGINXX_CAPABILITY_PHASE_MASK_*`
    uint32_t phases = PLUGINXX_CAPABILITY_PHASE_MASK_IMPL;
    /// 这次登记落在哪个阶段: `PLUGINXX_CAPABILITY_PHASE_*`
    int32_t phase = PLUGINXX_CAPABILITY_PHASE_IMPL;
    /// 行内顺序, 小者先
    int32_t priority = 0;
    /// 自报的最大耗时 (毫秒; 0 = 用声明里的值)
    int32_t timeoutMs = 0;
    /// 同一实例内区分多个处理器 (可空)
    std::string_view ownerTag{};
    /// 归属作用域 (空 = 进程级, 对所有作用域生效)
    const PluginxxScopeRef* scope = nullptr;
};

namespace detail {

/// 能力实现的上下文绑定 (跨边界 user_data)
template<typename Ctx, typename CapFn>
struct CapabilityShim {
    Ctx*                ctx = nullptr;
    std::decay_t<CapFn> fn;
};

/// 造一行能力登记的跨边界 spec (`run_start` / `cancel` 已绑定)
///
/// 业务体两种:
/// - 同步 (`void` / 字符串类): 调用返回即完成;
/// - `asio::awaitable<T>`: provider 根经桥的本地执行器推进。
///
/// `id` / `owner_tag` 的视图指向调用方给的字符串 (提交在主程序 IO 线程上内联执行,
/// 返回时 spec 已读完), 因此函数内不再复制。
template<typename Ctx, typename CapFn>
inline PluginxxCapabilitySpec buildCapabilitySpec(
    Ctx&                     ctx,
    std::string_view         capName,
    CapFn&&                  fn,
    const CapabilityOptions& opts
) {
    using Shim = CapabilityShim<Ctx, CapFn>;

    auto shim = ctx.storeShim(std::make_unique<Shim>(Shim{&ctx, std::forward<CapFn>(fn)}));

    PluginxxCapabilitySpec spec{};
    spec.version     = PLUGINXX_API_VERSION;
    spec.struct_size = sizeof(PluginxxCapabilitySpec);
    spec.id          = PluginStringView::from(capName.data(), capName.size());
    if (!opts.ownerTag.empty()) {
        spec.owner_tag = PluginStringView::from(opts.ownerTag.data(), opts.ownerTag.size());
    }
    spec.impls      = opts.impls;
    spec.merge      = opts.merge;
    spec.phases     = opts.phases;
    spec.phase      = opts.phase;
    spec.priority   = opts.priority;
    spec.timeout_ms = opts.timeoutMs;
    spec.user_data  = shim;

    spec.run_start = [](void*                         user_data,
                        const PluginxxHost*           caller_host,
                        const PluginxxStringView*     method,
                        const PluginxxStringView*     args_json,
                        const PluginxxOperatorNotify* notify,
                        PluginxxString*               error_out) -> void* {
        auto* s = static_cast<Shim*>(user_data);
        (void)error_out;
        if (!s || !s->ctx) {
            CompletionGuard guard(notify);
            guard.failed("capability context released");
            return nullptr;
        }
        // 能力入参纳入拥有型 Request: 业务只读到 Request 拥有的 method/args
        auto request = RootRequest::forCapability(s->ctx->host, method, args_json);

        using CapRet = decltype(
            invokeCap(s->fn, *s->ctx, caller_host, std::string_view{}, std::string_view{})
        );
        // 同步能力: void 或字符串类返回值; 其余 (asio 协程) 走异步路径。
        constexpr bool kSyncCap
            = std::is_void_v<CapRet> || std::is_convertible_v<CapRet, std::string_view>;
        if constexpr (kSyncCap) {
            /// 同步能力：调用返回即完成；异常统一映射为终态。
            CompletionGuard guard(notify);
            try {
                if constexpr (std::is_void_v<CapRet>) {
                    invokeCap(
                        s->fn,
                        *s->ctx,
                        caller_host,
                        request.capMethod(),
                        request.args()
                    );
                    guard.ok();
                } else {
                    guard.ok(invokeCap(
                        s->fn,
                        *s->ctx,
                        caller_host,
                        request.capMethod(),
                        request.args()
                    ));
                }
            } catch (...) {
                guard.fromCurrentException();
            }
            return nullptr;
        } else if constexpr (IsPluginAwaitable<CapRet>::value) {
            /// asio 协程能力: 起一条 provider 根; 入参由根保活到协程结束。
            auto hold = std::make_shared<RootRequest>(std::move(request));
            auto pr   = startProviderRoot(
                s->ctx->bridge(),
                notify ? *notify : PluginxxOperatorNotify{nullptr, nullptr},
                [s, caller_host, hold](std::shared_ptr<ProviderRoot> root) -> CapRet {
                    root->keepAlive = hold;
                    return invokeCap(
                        s->fn,
                        *s->ctx,
                        caller_host,
                        hold->capMethod(),
                        hold->args()
                    );
                },
                [](auto&& value) {
                    using ValueT = std::decay_t<decltype(value)>;
                    if constexpr (std::is_void_v<ValueT>) {
                        return std::make_pair(int32_t{PLUGINXX_OPERATOR_OK}, std::string{});
                    } else {
                        return std::make_pair(
                            int32_t{PLUGINXX_OPERATOR_OK},
                            std::string{value}
                        );
                    }
                },
                [](std::exception_ptr ep) {
                    std::string why;
                    try {
                        std::rethrow_exception(ep);
                    } catch (const std::exception& e) {
                        why = e.what();
                    } catch (...) {
                        why = "capability threw a non-standard exception";
                    }
                    return std::make_pair(int32_t{PLUGINXX_OPERATOR_FAILED}, why);
                }
            );
            return pr.get();
        } else {
            /// 返回类型对不上: 同步能力返回 `void` / 字符串, 异步能力返回
            /// `asio::awaitable<T>`
            static_assert(
                IsPluginAwaitable<CapRet>::value,
                "capability(): 业务体必须返回 void / 字符串 / asio::awaitable<T>"
            );
            CompletionGuard guard(notify);
            guard.failed("capability returned an unsupported type");
            return nullptr;
        }
    };

    spec.cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        static_cast<CancelableProvider*>(op)->requestCancel();
    };
    return spec;
}

} // namespace detail

/// 登记一行能力实现 (**唯一入口**: 提交/等待都用 asio 协程)
///
/// 业务体可返回字符串 (同步) 或 `asio::awaitable<T>` (异步): 返回值就是这次调用的
/// 结果文本。给工具体、功能埋点实现体这类 `asio::awaitable` 事务用。
template<typename Ctx, typename CapFn>
inline asio::awaitable<TableOpResult> capabilityAsync(
    Ctx&                     ctx,
    std::string_view         capName,
    CapFn&&                  fn,
    const CapabilityOptions& opts = {}
) {
    if (!ctx.iface.capabilities || !ctx.iface.capabilities->register_capability) {
        TableOpResult r;
        r.startError = "plugin capability iface unavailable";
        co_return r;
    }
    const auto spec = detail::buildCapabilitySpec(ctx, capName, std::forward<CapFn>(fn), opts);
    co_return co_await asyncTableOp(
        ctx,
        co_await asio::this_coro::executor,
        [&ctx, scope = opts.scope, spec](
            PluginxxOperatorCallback cb,
            void*                    ud,
            PluginxxString*          err
        ) -> PluginxxOperatorHandle* {
            return ctx.iface.capabilities->register_capability(
                ctx.host,
                scope,
                &spec,
                cb,
                ud,
                err
            );
        }
    );
}


/* ==================== 一键式插件入口导出宏 ==================== */

namespace detail {

/// 生命周期入口公共守卫 (导出宏内部使用):
/// 捕获跨 C ABI 的异常并写入 `err` (主程序回滚依据), 统一返回 NULL 表示"本次事务未同步成功"。
template<typename Fn>
inline void* callLifecycleEntry(
    const PluginxxHost*           host,
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err,
    const char*                   label,
    Fn&&                          fn
) noexcept {
    try {
        if (!plugin_ctx) {
            PluginString::set(host, err, fmt::format("{}: null plugin context", label));
            return nullptr;
        }
        if (!notify || !notify->done) {
            PluginString::set(host, err, fmt::format("{}: null completion notify", label));
            return nullptr;
        }
        return fn();
    } catch (const std::exception& e) {
        PluginString::set(host, err, e.what());
    } catch (...) {
        PluginString::set(host, err, fmt::format("{}: unknown exception", label));
    }
    return nullptr;
}

template<typename Ctx>
inline void autoStopSpawns(Ctx* ctx) noexcept {
    if constexpr (requires { ctx->stopSpawns(); }) {
        if (ctx) {
            ctx->stopSpawns();
        }
    }
}

/// 写插件元信息 (导出宏的 `get_info` 实现)
///
/// 框架给结构体并填好了 `api_version` / `struct_size`, 这里只写插件侧的字段:
/// `name` / `version` / `description` 借用字面量 (框架当场复制), `min_api_version` 是
/// 本 SDK 的头文件版本 (低版本主程序会直接拒绕)。
inline int32_t fillPluginInfo(
    PluginxxInfo* out,
    const char*   name,
    const char*   version,
    const char*   description,
    int32_t       minApiVersion
) noexcept {
    if (!out || out->struct_size < PLUGINXX_INFO_BASELINE_SIZE) {
        return -1;
    }
    out->min_api_version = minApiVersion;
    out->flags           = 0;
    out->name            = PluginStringView::fromCstr(name);
    out->version         = PluginStringView::fromCstr(version);
    out->description     = PluginStringView::fromCstr(description);
    return 0;
}

/// 生命周期失败收尾: 写错误串并直接 `done(FAILED, 原因)` (导出宏使用)
inline void notifyLifecycleFailure(
    const PluginxxHost*           host,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err,
    std::string_view              message
) noexcept {
    try {
        PluginString::set(host, err, message);
    } catch (...) {
    }
    if (notify && notify->done) {
        auto sv = PluginStringView::from(message.data(), message.size());
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &sv);
    }
}

/// 生命周期入口公共守卫 (void 版): 异常 / 参数错误都转成 `done(FAILED, 原因)`
///
/// 与 [callLifecycleEntry] 同一道守卫, 只是不再用"返回值"表达结果 ——
/// 新协议的入口一律 `void` + 完成通知。
template<typename Fn>
inline void callLifecycleEntryVoid(
    const PluginxxHost*           host,
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err,
    const char*                   label,
    Fn&&                          fn
) noexcept {
    if (!notify || !notify->done) {
        try {
            PluginString::set(host, err, fmt::format("{}: null completion notify", label));
        } catch (...) {
        }
        return;
    }
    if (!plugin_ctx) {
        notifyLifecycleFailure(host, notify, err, fmt::format("{}: null plugin context", label));
        return;
    }
    try {
        fn();
    } catch (const std::exception& e) {
        notifyLifecycleFailure(host, notify, err, e.what());
    } catch (...) {
        notifyLifecycleFailure(host, notify, err, fmt::format("{}: unknown exception", label));
    }
}

/// 实例上下文是否具备 kit 基类形态 (`PluginBaseT` 派生): 能 `attachHost`, 且业务
/// 初始化是伸程 (`onHostReady`)
///
/// 不是 kit 上下文的自定义上下文 (如 `example_js_execute_command` 的 `ShellCtx`)
/// 自己提供 `init(host)`, 由 create 事务保持旧的同步路径。
template<typename C>
concept KitInstanceCtx = requires(C& c, const PluginxxHost* h) {
    c.attachHost(h);
    c.onHostReady();
};

/// 实例创建入口的公共实现: 构造上下文 (**可重入**) + 业务初始化 + 注册事务
///
/// - `*plugin_ctx` 非空 = 同一实例的第二次 `create` (禁用后再启用): 不重建上下文,
///   不重跑业务初始化, 只跑注册事务;
/// - 首次构造: 同步阶段只做“绑定 host/iface” (`attachHost`, `spawn` 依赖它) 与写出
///   `*plugin_ctx`; 需要调表的业务初始化 (`onHostReady`) 与注册事务进后台任务,
///   完成后经 `notify` 恰好一次交回 —— 插件执行线程上不能同步等表;
/// - 构造失败、初始化/注册抛异常都按失败收尾 (`done(FAILED, 原因)`), 调用方 (主程序)
///   负责回滚;
/// - `createFn` 为空 = 只构造 (插件没有注册事务);
/// - 主机没有 `pluginxx.tasks` 表时 `spawn` 不可用, 退回“只绑定 + 注册事务” (跳过
///   `onHostReady`) 并记一条警告 —— 这种主机连 `finishLater` 也用不了;
/// - `name` 只用于诊断 (构造失败时写进错误串) —— 不能在这里打日志:
///   本头不依赖主程序的日志设施 (框架的通用 `log` 表要等实例构造完成才能用)。
template<typename CtxType>
inline void createInstanceEntry(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err,
    const char*                   name,
    void (*createFn)(CtxType&, const PluginxxOperatorNotify*, PluginxxString*)
) noexcept {
    if (!plugin_ctx) {
        notifyLifecycleFailure(host, notify, err, "plugin create: null plugin context");
        return;
    }
    if (!notify || !notify->done) {
        pluginxx::PluginString::set(host, err, "plugin create: null completion notify");
        return;
    }
    auto*      ctx   = static_cast<CtxType*>(*plugin_ctx);
    const bool fresh = (ctx == nullptr);
    if (fresh) {
        try {
            auto instance = std::make_unique<CtxType>();
            if constexpr (KitInstanceCtx<CtxType>) {
                instance->attachHost(host);
            } else {
                // 自定义上下文: 自己绑定 host / 接口表 (旧形态)
                instance->init(host);
            }
            ctx         = instance.release();
            *plugin_ctx = ctx;
        } catch (const std::exception& e) {
            notifyLifecycleFailure(
                host,
                notify,
                err,
                fmt::format("plugin create{}: {}", name ? name : "", e.what())
            );
            return;
        } catch (...) {
            notifyLifecycleFailure(host, notify, err, "plugin create: unknown exception");
            return;
        }
    }
    // 首次构造: 业务初始化是协程 (要调表), 与注册事务一起进后台任务。
    if constexpr (KitInstanceCtx<CtxType>) {
        if (fresh && ctx->iface.tasks && ctx->iface.tasks->register_task) {
            const PluginxxOperatorNotify done = *notify; // 借用指针: 按值持有到任务结束
            const bool                   scheduled
                = ctx->spawn([host, done, createFn](CtxType& instance, OpCtl&) -> asio::awaitable<void> {
                try {
                    co_await instance.onHostReady();
                    if (createFn) {
                        // 注册事务可能在入口内部分发起异步工作 (finishLater), 自己完成通知;
                        // 这里只负责把同步写出的错误串释放掉
                        PluginxxString localError{nullptr, 0};
                        createFn(instance, &done, &localError);
                        if (localError.data) {
                            pluginxx::PluginString::free(host, &localError);
                        }
                    } else {
                        done.done(done.host_ud, PLUGINXX_OPERATOR_OK, nullptr);
                    }
                } catch (const std::exception& e) {
                    notifyLifecycleFailure(host, &done, nullptr, e.what());
                } catch (...) {
                    notifyLifecycleFailure(host, &done, nullptr, "plugin create: unknown exception");
                }
                co_return;
            });
            if (!scheduled) {
                // 任务没排上 (register_task 拒绝): 协程体一行都不会执行, 完成通知也就
                // 永远不会发出 —— 这里补一次失败, 不让主程序等到关闭截止时间
                notifyLifecycleFailure(
                    host,
                    notify,
                    err,
                    "plugin create: could not schedule the create transaction"
                );
            }
            return;
        }
        if (fresh) {
            ctx->log.warn(
                "plugin create: host has no pluginxx.tasks; business init (onHostReady) skipped"
            );
        }
    }
    callLifecycleEntryVoid(host, ctx, notify, err, "plugin create", [&]() {
        if (createFn) {
            createFn(*ctx, notify, err);
        } else {
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        }
    });
}

/// 实例销毁入口的公共实现: 释放本地对象并交回完成通知
///
/// `cleanup` 只做本地释放 (不调主程序接口、不起异步工作); 完成后主程序才 `dlclose`。
template<typename CtxType, typename CleanupFn>
inline void destroyInstanceEntry(
    const PluginxxHost*           host,
    CtxType*                      ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err,
    CleanupFn&&                   cleanup
) noexcept {
    try {
        cleanup(ctx);
    } catch (const std::exception& e) {
        notifyLifecycleFailure(host, notify, err, e.what());
        return;
    } catch (...) {
        notifyLifecycleFailure(host, notify, err, "plugin destroy: unknown exception");
        return;
    }
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}

} // namespace detail

/// create 阶段异常上报 (上下文可能尚未构造成功, 直接经主程序日志接口输出)
inline void logCreateFailure(
    const PluginxxHost* host,
    std::string_view    plugin,
    std::string_view    msg
) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface) {
        return;
    }
    auto  iid   = PluginStringView::fromCstr(PLUGINXX_IFACE_LOG);
    auto* iface = static_cast<const PluginxxLogIface*>(host->vtable->query_interface(host, &iid));
    if (!iface || !iface->log) {
        return;
    }
    std::string text = fmt::format("[{}] create failed: {}", plugin, msg);
    auto        sv   = PluginStringView::from(text.data(), text.size());
    iface->log(host, 4, &sv);
}

/// 入口符号名拼接: `PLUGINXX_ENTRY_SYMBOL(<前缀>_plugin_, create)` → `<前缀>_plugin_create`
///
/// 符号前缀由主程序给出 (插件框架不含任何主程序专名); 主程序在自己的 SDK 头里用它包出对插件作者
/// 友好的导出宏 (主程序可包出自己的名字, 如 `<主程序前缀>_PLUGIN_<端>_EXPORT`)。
#define PLUGINXX_ENTRY_SYMBOL_JOIN__(Prefix, Suffix) Prefix##Suffix
#define PLUGINXX_ENTRY_SYMBOL_JOIN_(Prefix, Suffix)  PLUGINXX_ENTRY_SYMBOL_JOIN__(Prefix, Suffix)
#define PLUGINXX_ENTRY_SYMBOL(Prefix, Suffix)        PLUGINXX_ENTRY_SYMBOL_JOIN_(Prefix, Suffix)

/// 插件入口导出 (生成 `<SymbolPrefix>get_info/create/stop/destroy` 四个符号)。
///
/// `SymbolPrefix` 是主程序命名空间前缀 (形如 `<应用名>_<端>_plugin_`)。
/// 完整形态是 `<应用名>_agent_plugin_` / `<应用名>_client_plugin_`; 运行时主程序必须经
/// `entrySymbols()` 交出同一批符号名 (见 `pluginxx/api/entry.h`), 两侧不一致时加载失败。
///
/// 生命周期只有一个协议: **调用 → 恰好一次 `notify->done`**。
/// - `get_info`: 框架给结构体 (`PluginxxInfo`), 插件只填名字/版本/描述与最低版本;
/// - `create`: 构造上下文 + 做注册事务。上下文走 `void**` 出参且必须同步写出;
///   可重入 (同一实例第二次 create 时 `*plugin_ctx` 已经是上一次的上下文,
///   于是只重新注册, 不重建对象);
/// - `stop`: 撤销注册与自管资源 (线程/定时器/订阅), 可重复调用, 上下文保留;
/// - `destroy`: 释放本地对象 (可慢); 完成后主程序才在自己的工作线程上 `dlclose`。
///
/// 插件只需给出两个业务函数:
/// - `CreateFn`: 注册事务, 形如 `void(CtxType&, const PluginxxOperatorNotify*, PluginxxString*)`,
///   参数与旧的 `start` 完全一样 (同步完成时先 `notify->done(OK)` 再返回);
/// - `StopFn`: 撤销事务, 同一形态。
///
/// 上下文构造被包在 `create` 里 (`new CtxType` + `init(host)`), 因此插件不能再把
/// "创建对象"和"注册"分开写 —— 也不再需要"create 不许读表/不许起线程"这类无法执行的约定。
#define PLUGINXX_EXPORT_PLUGIN(SymbolPrefix, CtxType, Name, Ver, Desc, CreateFn, StopFn)         \
    extern "C" PLUGINXX_EXPORT int32_t PLUGINXX_CALL                                             \
        PLUGINXX_ENTRY_SYMBOL(SymbolPrefix, get_info)(PluginxxInfo* out) {                        \
        return pluginxx::detail::fillPluginInfo(out, Name, Ver, Desc, PLUGINXX_MIN_API_VERSION);  \
    }                                                                                             \
    extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL PLUGINXX_ENTRY_SYMBOL(SymbolPrefix, create)(    \
        const PluginxxHost*           host,                                                       \
        void**                        plugin_ctx,                                                 \
        const PluginxxOperatorNotify* notify,                                                     \
        PluginxxString*               err                                                         \
    ) {                                                                                           \
        auto createFn = static_cast<                                                              \
            void (*)(CtxType&, const PluginxxOperatorNotify*, PluginxxString*)>(CreateFn);         \
        pluginxx::detail::createInstanceEntry<CtxType>(                                           \
            host,                                                                                 \
            plugin_ctx,                                                                           \
            notify,                                                                               \
            err,                                                                                  \
            Name,                                                                                 \
            createFn                                                                              \
        );                                                                                        \
    }                                                                                             \
    extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL PLUGINXX_ENTRY_SYMBOL(SymbolPrefix, stop)(      \
        void*                         plugin_ctx,                                                 \
        const PluginxxOperatorNotify* notify,                                                     \
        PluginxxString*               err                                                         \
    ) {                                                                                            \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                            \
        auto  stopFn = static_cast<                                                                 \
            void (*)(CtxType&, const PluginxxOperatorNotify*, PluginxxString*)>(StopFn);           \
        pluginxx::detail::callLifecycleEntryVoid(                                                 \
            ctx ? ctx->host : nullptr,                                                            \
            plugin_ctx,                                                                           \
            notify,                                                                               \
            err,                                                                                  \
            "plugin stop",                                                                        \
            [&]() {                                                                               \
                pluginxx::detail::autoStopSpawns(ctx);                                            \
                if (stopFn) {                                                                     \
                    stopFn(*ctx, notify, err);                                                    \
                } else {                                                                          \
                    /* 没有自定义 stop 事务, 仍要给出完成信号 */                                 \
                    if (notify && notify->done) {                                                 \
                        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);             \
                    }                                                                             \
                }                                                                                 \
            }                                                                                     \
        );                                                                                        \
    }                                                                                             \
    extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL PLUGINXX_ENTRY_SYMBOL(SymbolPrefix, destroy)(   \
        void*                         plugin_ctx,                                                 \
        const PluginxxOperatorNotify* notify,                                                     \
        PluginxxString*               err                                                         \
    ) {                                                                                            \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                            \
        pluginxx::detail::destroyInstanceEntry<CtxType>(                                          \
            ctx ? ctx->host : nullptr,                                                            \
            ctx,                                                                                  \
            notify,                                                                               \
            err,                                                                                  \
            [](CtxType* c) {                                                                      \
                if (!c) {                                                                         \
                    return;                                                                       \
                }                                                                                 \
                pluginxx::detail::autoStopSpawns(c);                                              \
                delete c;                                                                         \
            }                                                                                     \
        );                                                                                        \
    }

/// 只导出 `stop` 入口 (供手写 create/destroy 的插件使用)。
///
/// 少数插件需要自己控制实例构造或销毁 (如按平台条件创建运行时对象、加自定义日志),
/// 它们手写 `<SymbolPrefix>create` / `<SymbolPrefix>destroy`, 但仍用本宏生成带异常保护的
/// stop trampoline —— 与 [PLUGINXX_EXPORT_PLUGIN] 的 stop 部分同语义。
#define PLUGINXX_EXPORT_PLUGIN_LIFECYCLE(SymbolPrefix, CtxType, StopFn)                          \
    extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL PLUGINXX_ENTRY_SYMBOL(SymbolPrefix, stop)(     \
        void*                         plugin_ctx,                                                \
        const PluginxxOperatorNotify* notify,                                                    \
        PluginxxString*               err                                                        \
    ) {                                                                                          \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                           \
        auto  stopFn = static_cast<                                                                \
            void (*)(CtxType&, const PluginxxOperatorNotify*, PluginxxString*)>(StopFn);          \
        pluginxx::detail::callLifecycleEntryVoid(                                                \
            ctx ? ctx->host : nullptr,                                                           \
            plugin_ctx,                                                                          \
            notify,                                                                              \
            err,                                                                                 \
            "plugin stop",                                                                       \
            [&]() {                                                                              \
                pluginxx::detail::autoStopSpawns(ctx);                                           \
                if (stopFn) {                                                                    \
                    stopFn(*ctx, notify, err);                                                   \
                } else if (notify && notify->done) {                                             \
                    /* 没有自定义 stop 事务, 仍要给出完成信号 */                                 \
                    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);                \
                }                                                                                \
            }                                                                                    \
        );                                                                                       \
    }
} // namespace pluginxx
