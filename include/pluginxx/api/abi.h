/// pluginxx 纯 C ABI 基座 (与宿主领域无关的跨边界契约)
///
/// 内容:
/// - 导出符号控制 (`PLUGINXX_EXPORT`) 与调用约定 (`PLUGINXX_CALL`)
/// - 全局 API 版本、结构体对齐 (8 字节)、定长基础类型约定
/// - 跨边界字符串类型 (`PluginxxStringView` / `PluginxxString`)
/// - 统一异步操作原语 (完成通知器 / 回调 / 取消令牌 / 句柄)
/// - 核心宿主函数表 (`PluginxxHostVtable` / `PluginxxHost`)
/// - 入口函数指针类型、内置合并编译的描述结构
///   (入口**符号名**由宿主提供, 见 `pluginxx/api/entry.h`)
///
/// 契约稳定性: 本头中的 **C 符号名 / 结构体名 / 宏名 / IID 字符串一律冻结**,
/// 改动即破坏已编译的插件二进制。领域接口表 (工具/权限/钩子/会话/模型/提示词/
/// 资源/图 等与宿主领域相关的表) 由宿主自行定义; 本头只承载与领域无关的通用部分。
#ifndef PLUGINXX_API_ABI_H
#define PLUGINXX_API_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 结构体 8 字节对齐由下方"插件导出符号控制"段落的 #pragma pack(push, 8) 打开 */


/* ==================== 插件导出符号控制与调用约定 ==================== */

#if defined(PLUGINXX_BUILTIN)
#define PLUGINXX_EXPORT
#elif defined(_WIN32)
#define PLUGINXX_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define PLUGINXX_EXPORT __attribute__((visibility("default")))
#else
#define PLUGINXX_EXPORT
#endif

#if defined(_WIN32)
#define PLUGINXX_CALL __stdcall
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__i386__)
#define PLUGINXX_CALL __attribute__((stdcall))
#else
#define PLUGINXX_CALL
#endif
#else
#define PLUGINXX_CALL
#endif

/// 全局 API 版本 (agent 侧)
#define PLUGINXX_API_VERSION 1

#pragma pack(push, 8)

/* ==================== 字符串视图 (跨边界只读参数统一形态) ==================== */

/// 只读字符串视图: 指向调用方内存 (UTF-8), 不要求 NUL 结尾
/// - C ABI: data(8) + size(8) 纯 POD, 恒按指针/出参传递 (不按值跨边界)
typedef struct PluginxxStringView {
    const char* data; ///< 指向 UTF-8 字节序列 (可含任意字节, 不必 NUL 结尾)
    uint64_t    size; ///< 字节数 (明确定长 64 位)
} PluginxxStringView;

typedef struct PluginxxHost PluginxxHost;

/* ==================== 跨边界堆分配字符串 (具有显式所有权) ==================== */

/// 跨 CRT 堆分配的 UTF-8 字符串 (显式所有权: 由宿主分配, 调用方接管并负责释放)
typedef struct PluginxxString {
    char* data; ///< 指向宿主堆分配的 UTF-8 字节序列 (以 \0 结尾; 空串或 NULL 时可为 NULL)
    uint64_t size; ///< 字节数 (不含结尾 \0; O(1) 访问)
} PluginxxString;

/// ==================== 插件元信息 ====================

typedef struct PluginxxInfo {
    int32_t                 api_version; ///< 必须 >= PLUGINXX_API_VERSION
    uint32_t                _reserved;   ///< 8 字节补齐
    PluginxxStringView name;        ///< 唯一标识 (只读借用)
    PluginxxStringView version;
    PluginxxStringView description;
} PluginxxInfo;

/// ==================== 统一异步操作原语 ====================

/// 操作终结状态 (PluginxxOperatorNotify.done 的 status 参数)
#define PLUGINXX_OPERATOR_OK        0 ///< 成功 (payload = 结果数据)
#define PLUGINXX_OPERATOR_CANCELLED 1 ///< 已取消 (payload 可为 NULL/空)
#define PLUGINXX_OPERATOR_FAILED    2 ///< 失败 (payload = 错误信息)

/// 完成通知器 (宿主实现并随 start 下发; 操作终结时被调方须【恰好回调一次】)
/// - payload: 只读借用字符串视图指针 (可为 NULL/空)
/// - 线程安全: 可从被调方的任意线程回调, 宿主内部投递回 io 线程唤醒等待协程
typedef struct PluginxxOperatorNotify {
    void(PLUGINXX_CALL*
             done)(void* host_ud, int32_t status, const PluginxxStringView* payload);
    void* host_ud;
} PluginxxOperatorNotify;

/// 完成回调 (统一形态; 宿主保证在宿主 io 线程派发)
/// payload 只读借用指针, 生命周期仅覆盖本次回调
typedef void(PLUGINXX_CALL* PluginxxOperatorCallback)(
    void*                          ud,
    int32_t                        status,
    const PluginxxStringView* payload
);

/// 异步调用句柄 (仅用于取消; 不可轮询/收尸; 宿主托管生命周期)
typedef struct PluginxxOperatorHandle PluginxxOperatorHandle;

/// 单次驱动请求 (宿主托管生命周期; 插件只持有裸指针用于取消)
///
/// 语义 (见 `pluginxx.coroutine_runtime` 接口表):
/// - 一次 ticket 至多执行一次 drive_once 回调, 且永不内联执行 (宿主异步投递);
/// - ticket 持有插件实例的执行 lease, 因此 dlclose 不会越过它;
/// - 宿主保证 `cancel_driver` 之后该 ticket 不再执行回调。
typedef struct PluginxxDriver PluginxxDriver;

/// 驱动回调: 插件在此推进本地运行时一个有限步骤
/// - **不得阻塞、不得等待事件、不得同步调用宿主业务接口**;
/// - 在宿主 IO 线程执行 (可用 `is_io_thread` 校验);
/// - 异常必须由插件自行捕获 (跨越 C ABI 的异常是未定义行为)。
typedef void(PLUGINXX_CALL* PluginxxDriveOnceFn)(void* user_data);

/// 不透明协作式取消令牌。
/// 令牌只在当前受管工作函数执行期间有效；插件不得保存该指针。
typedef struct PluginxxCancelToken PluginxxCancelToken;
typedef int32_t(PLUGINXX_CALL* PluginxxCancelIsRequestedFn)(
    const PluginxxCancelToken* token
);

struct PluginxxCancelToken {
    PluginxxCancelIsRequestedFn is_requested;
    void*                            host_ud;
};

static inline int32_t pluginxx_cancel_is_requested(const PluginxxCancelToken* token) {
    return token && token->is_requested ? token->is_requested(token) : 0;
}

/// 协作式取消请求函数 (【宿主 io 线程调用】, 非阻塞):
typedef void(PLUGINXX_CALL* PluginxxOperatorCancelFunction)(void* user_data, void* op);

/* ==================== 事件订阅句柄 / 前向声明 ==================== */

typedef struct PluginxxSubscription PluginxxSubscription;

/// 能力方法处理器启动函数 (操作契约):
typedef void*(PLUGINXX_CALL* PluginxxCapabilityStartFunction)(
    void*                              ctx,
    const PluginxxHost*           caller_host,
    const PluginxxStringView*     method,
    const PluginxxStringView*     args_json,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
);

/* ==================== 核心宿主函数表 ==================== */

/// 核心 vtable: 极简正交基 (内存操作 + COM 风格接口表查询)
typedef struct PluginxxHostVtable {
    /* ---- 内存 (跨 CRT 堆边界的唯一分配通道; 任意线程可调用) ---- */
    void*(PLUGINXX_CALL* alloc)(uint64_t size);
    void(PLUGINXX_CALL* free)(void* ptr);

    /* ---- COM 风格接口表查询 (QueryInterface; 任意线程可调用) ---- */
    const void*(PLUGINXX_CALL* query_interface)(
        const PluginxxHost*       host,
        const PluginxxStringView* iid
    );
} PluginxxHostVtable;

struct PluginxxHost {
    const PluginxxHostVtable* vtable; ///< 核心函数表 (宿主静态)
    void* opaque; ///< 宿主内部 (指向插件实例状态, 插件不得使用)
};

/* ==================== 插件入口符号 (dlsym) ==================== */

typedef const PluginxxInfo*(PLUGINXX_CALL* PluginxxGetInfoFn)(void);
typedef int32_t(PLUGINXX_CALL* PluginxxCreateFn)(
    const PluginxxHost* host,
    void**                   plugin_ctx
);
typedef void(PLUGINXX_CALL* PluginxxDestroyFn)(void* plugin_ctx);

/// 实例生命周期入口 (必备, 见 plugin_kit.h 的导出宏):
/// - create 只构造上下文, 不做注册、不起线程;
/// - start 是注册事务, 在插件所属 IO executor 上执行;
/// - stop 撤销自管资源, 宿主在 Closing 阶段调用, 完成后才调用 destroy。
typedef void*(PLUGINXX_CALL* PluginxxStartFn)(
    void*                              plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
);
typedef void*(PLUGINXX_CALL* PluginxxStopFn)(
    void*                              plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
);

/* ==================== 插件入口符号名 (由宿主定义, 见 pluginxx/api/entry.h) ==================== */

/// 内核**不**定义入口符号名常量: 符号名属于宿主命名空间 (agentxx 用
/// `agentxx_plugin_agent_*` / `agentxx_plugin_client_*`, musicxx 用 `musicxx_plugin_*`)。
/// - 宿主侧: 覆写 `pluginxx::PluginHostLifecycle::entrySymbols()` 交出符号名;
/// - 插件侧: 用 `PLUGINXX_EXPORT_PLUGIN(SymbolPrefix, ...)` (见 kit.h) 生成入口。

/// 内置插件描述 (合并编译进宿主二进制的插件; 静态数组, 进程生命周期有效)
typedef struct PluginxxBuiltinInfo {
    PluginxxStringView name; ///< 插件唯一名 (如 "example_plugin"); NULL = 空表占位
    PluginxxGetInfoFn get_info; ///< 可空 (加载前元信息校验, 与 dlsym 可选符号同语义)
    PluginxxCreateFn create; ///< 必需 (实例创建, 与 get_info 同一导出宏生成)
    PluginxxDestroyFn destroy; ///< 可空 (实例销毁)
    PluginxxStartFn start; ///< 可空 (create 后的注册/启动事务)
    PluginxxStopFn  stop;  ///< 可空 (关闭事务, destroy 前调用)
} PluginxxBuiltinInfo;

// 与 PluginxxBuiltinInfo 同步生成于宿主侧的内置插件清单源码
typedef struct PluginxxBuiltinManifest {
    PluginxxStringView name; ///< 插件名
    PluginxxStringView yaml; ///< plugin.yaml 原文 (UTF-8, 静态只读)
} PluginxxBuiltinManifest;

/* ==================== 内置插件清单 (宿主提供) ==================== */

/// 内置插件 (合并编译进宿主二进制的插件) 的描述数组与内嵌清单数组由**宿主**定义,
/// 并在启动早期经 `pluginxx::setBuiltinPluginProvider` 注册给内核
/// (见 `pluginxx/host/manifest.h`); 内核因此不引用任何宿主导出的符号。

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* PLUGINXX_API_ABI_H */
