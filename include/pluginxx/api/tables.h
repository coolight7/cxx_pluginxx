/// pluginxx 通用接口表 (与主程序业务无关的跨边界 C ABI 表)
///
/// 收录: 事件 / 能力 / 任务调度 / 协程驱动 / 插件互查 / 主程序配置 / 会话取消状态 /
/// JSON 辅助 / 日志 / 后台任务。
///
/// 归属判据: **只需要 JSON 载荷与通用资源即可工作** 的表属于通用表 (本头);
/// 需要主程序业务语义 (工具/权限/钩子/会话/模型/提示词/资源/执行图, 以及 client
/// 侧 UI/事件/会话/线路) 的表由主程序定义与实现。
///
/// 契约稳定性: 全部 C 名、结构体名、表名冻结 (见 abi.h 说明)。
/// **通用表的 IID 由 `PLUGINXX_APP_ID("<表名>")` 拼出**: 表名部分冻结, 应用名由构建配置
/// 给出 (CMake 缓存变量 `PLUGINXX_APP_NAME`, 见 `pluginxx/api/app.h`), 所以同一份源码
/// 在不同主程序里编出来是 `<应用名>.<表名>`。
///
/// ════════════════════════════════════════════════════════════════════
/// 接口表的版本与布局规则 (本头每张表都适用)
/// ════════════════════════════════════════════════════════════════════
/// 每张表四项齐全 (缺一不可):
/// - **IID** (`PLUGINXX_IFACE_<名>`): `query_interface` 的键; 换契约 = 换 IID;
/// - **`version`** 字段 + `_VERSION`: 本头描述的完整布局与语义级别; 追加成员或
///   语义变化时 +1; 纯注释变化不动;
/// - **`_MIN_VERSION`**: 本头编写的代码能与之配合工作的最低表版本; 只有"同一 IID
///   内不再兼容旧布局/旧语义"时才 +1 (会拒掉老主程序, 优先换 IID);
/// - **`_BASELINE_SIZE`**: 表头之后**所有提供本表的主程序都应具备**的字节数;
///   重置时 = `sizeof(表)`, 以后追加成员时保持不变 (值改为第一个追加成员的偏移)。
///
/// 判定规则:
/// - 整表可用 = IID 命中 且 `version >= _MIN_VERSION` 且 `struct_size >= _BASELINE_SIZE`;
/// - 某成员可用 = 整表可用 且 (基线以内的成员自动可用; 追加段成员用
///   `pluginxx::ifaceHas<&表::成员>(t)` 判定, 见 `pluginxx/api/iface_meta.h`)。
///
/// 改动规则:
/// - **成员只能追加在表尾**; 追加时 `_VERSION` +1、`_BASELINE_SIZE` 不变、调用点改用
///   `ifaceHas`; 语义变化 (布局不变) 时 `_VERSION` +1 并按需用 `ifaceHasVersion` 判定;
/// - **禁止**重排成员、改成员签名、删除成员 (结构体大小可能不变, 闸门挡不住, 会静默
///   调到隔壁函数) —— 要改就换新 IID;
/// - 用 `_MIN_VERSION` 表达"我加了新成员"是错的 (那等于把老主程序全部判死)。
///
/// 本表的当前状态: 通用表 10 张。其中 `capabilities` 为 v5 (v4 是声明 + 多实现行,
/// v5 在表尾追加了 `invoke_capability_ex`), `broadcasts` 为 v1 (取代旧的事件表),
/// `config` / `cancel` 为 v3, 其余 6 张仍为 v1。
///
/// 接口契约的代次 (全局): v4 = 能力改成"声明 + 实现行" (阶段 / 优先级 / 覆盖)、
/// 事件表换成广播表 (频道声明 + 缓存 + 写权限); v3 = 插件一次加载、可挂到多个作用域
/// (作用域由主程序定义, 经 [PluginxxScopeRef] 传递), 且**主程序业务语义不进入通用表**
/// (工具提示词 / 会话工作目录 / 会话取消状态一类的成员已交回主程序自己的业务表)。
/// 更早的插件由 `PLUGINXX_MIN_API_VERSION` 挡在加载阶段。
#ifndef PLUGINXX_API_TABLES_H
#define PLUGINXX_API_TABLES_H

#include "pluginxx/api/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

/* ==================== 能力与广播的枚举值 (v4 / v1 起) ==================== */

/* 一个 id 上有几个实现 (`PluginxxCapabilitySpec.impls`) */
#define PLUGINXX_CAPABILITY_IMPLS_ONE  0
#define PLUGINXX_CAPABILITY_IMPLS_MANY 1

/* 实现链的结果怎么合并 (`PluginxxCapabilitySpec.merge`; 0 = 不适用) */
#define PLUGINXX_CAPABILITY_MERGE_NONE  0
#define PLUGINXX_CAPABILITY_MERGE_FIRST 1
#define PLUGINXX_CAPABILITY_MERGE_VETO  2

/* 声明开放的阶段 (`PluginxxCapabilitySpec.phases`, 位或) */
#define PLUGINXX_CAPABILITY_PHASE_MASK_IMPL   (1u << 0)
#define PLUGINXX_CAPABILITY_PHASE_MASK_BEFORE (1u << 1)
#define PLUGINXX_CAPABILITY_PHASE_MASK_AFTER  (1u << 2)

/* 一次登记落在哪个阶段 (`PluginxxCapabilitySpec.phase`, 互斥的一个值) */
#define PLUGINXX_CAPABILITY_PHASE_IMPL   1
#define PLUGINXX_CAPABILITY_PHASE_BEFORE 2
#define PLUGINXX_CAPABILITY_PHASE_AFTER  4

/* 频道有没有当前值 (`PluginxxBroadcastDecl.cache`) */
#define PLUGINXX_BROADCAST_CACHE_NONE   0
#define PLUGINXX_BROADCAST_CACHE_LATEST 1

/* 谁能发布 (`PluginxxBroadcastDecl.publish`) */
#define PLUGINXX_BROADCAST_PUBLISH_OWNER  0
#define PLUGINXX_BROADCAST_PUBLISH_ANYONE 1

/* 频道用途 (`PluginxxBroadcastDecl.scope`) */
#define PLUGINXX_BROADCAST_SCOPE_APP    0
#define PLUGINXX_BROADCAST_SCOPE_SYSTEM 1

/* ==================== 接口表: 广播 (<应用名>.broadcasts) ==================== */

#define PLUGINXX_IFACE_BROADCASTS         PLUGINXX_APP_ID("broadcasts")
/// v1: 频道声明 + 发布 + 订阅 + 读当前值 (取代 `pluginxx.events` v3)
/// v2: `subscribe_broadcast` 多一个 `options_json` (每个订阅方自己声明通知频率);
///     声明字段 `writable` 改名 `publish`, 宏改成 `PLUGINXX_BROADCAST_PUBLISH_*`
/// v3: 表尾追加 `end_broadcast` (属主结束一条流) 与 `subscribe_broadcast_ex`
///     (订阅时带结束回调); 基线不变, 调用点用 `ifaceHas` 判定
#define PLUGINXX_IFACE_BROADCASTS_VERSION     3
#define PLUGINXX_IFACE_BROADCASTS_MIN_VERSION 2
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_BROADCASTS_BASELINE_SIZE \
    (offsetof(PluginxxBroadcastsIface, end_broadcast))

/// 广播的通用结果码 (v1 起; 与主程序自己的错误码分开, 只在 uint 范围内比较)
#define PLUGINXX_RESULT_OK               0
#define PLUGINXX_RESULT_ERR_ARG         -1 ///< 参数非法
#define PLUGINXX_RESULT_ERR_NOT_DECLARED -2 ///< 频道/能力没有声明
#define PLUGINXX_RESULT_ERR_PERMISSION  -3 ///< 没有发布权限 (非属主 / 系统频道)
#define PLUGINXX_RESULT_ERR_NO_VALUE    -4 ///< 没有当前值 (cache: none 或还没写过)
#define PLUGINXX_RESULT_ERR_ENDED       -5 ///< 流已结束 (发布 / 读取 / 订阅都被拒)

typedef struct PluginxxBroadcastsIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_BROADCASTS_VERSION
    uint32_t struct_size;

    /// 声明频道 (**任意线程**)
    /// - `decl_json` 形如
    ///   `{"id": "...", "cache": "latest", "publish": "owner",
    ///     "pushOnSubscribe": true, "coalesce": true, "scope": "app"}`
    ///   (`id` 可用短名: 宿主补成 `plugin.<本插件 id>.<名字>`);
    /// - 应用已声明的 id 不能被插件改写; 插件自己的 id 后声明者覆盖;
    /// - 失败返回 [PLUGINXX_RESULT_ERR_ARG] / [PLUGINXX_RESULT_ERR_PERMISSION] 并写 `error_out`
    int32_t(PLUGINXX_CALL* declare_broadcast)(
        const PluginxxHost*       host,
        const PluginxxStringView* decl_json,
        PluginxxString*           error_out
    );

    /// 发布 (**任意线程**): 属主发 `publish: owner` 的频道, 任何插件发 `publish: anyone` 的频道
    int32_t(PLUGINXX_CALL* publish_broadcast)(
        const PluginxxHost*       host,
        const PluginxxStringView* broadcast_id,
        const PluginxxStringView* payload_json,
        PluginxxString*           error_out
    );

    /// 订阅 (**任意线程**)
    /// - `pushOnSubscribe` 的频道在返回值之前用当前值回调一次 (没有当前值时不回调),
    ///   这次回调算该订阅的第一条通知, 也是限频窗口的起点;
    /// - `options_json` 可空, 形如 `{"intervalMs": 200, "mode": "delay|drop"}`:
    ///   `intervalMs` = 通知间隔 (毫秒, 0 / 缺省 = 不限频), `mode` = 间隔内只留最新 (delay) /
    ///   直接丢掉 (drop);
    /// - 失败返回 NULL 并写 `error_out` (句柄用 [unsubscribe_broadcast] 撤销)
    PluginxxSubscription*(PLUGINXX_CALL* subscribe_broadcast)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* broadcast_id,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payload_json, void* ud),
        void*                     ud,
        const PluginxxStringView* options_json,
        PluginxxString*           error_out
    );

    /// 读当前值 (同步): 成功返回 [PLUGINXX_RESULT_OK] 并写 `out_json`;
    /// 没有声明返回 [PLUGINXX_RESULT_ERR_NOT_DECLARED], 没有当前值返回 [PLUGINXX_RESULT_ERR_NO_VALUE]
    int32_t(PLUGINXX_CALL* read_broadcast)(
        const PluginxxHost*       host,
        const PluginxxStringView* broadcast_id,
        PluginxxString*           out_json
    );

    /// 撤销订阅 (**幂等**, 任意线程)
    void(PLUGINXX_CALL* unsubscribe_broadcast)(PluginxxSubscription* sub);

    /// 结束一条流 (追加成员, v3)
    ///
    /// - `writer` 由框架按调用实例的插件 id 代; 应用自己结束走 C++ 层
    ///   (`PluginHostCore::endBroadcast`) 或传空的 writer;
    /// - 结束后: 发布返回 [PLUGINXX_RESULT_ERR_ENDED]、`read` 也返回它、
    ///   全部订阅按 `on_end` 回调一次并摧除;
    /// - 重复结束是幂等成功; 再次 `declare` 同一个 id = 开一条新流。
    int32_t(PLUGINXX_CALL* end_broadcast)(
        const PluginxxHost*       host,
        const PluginxxStringView* broadcast_id,
        const PluginxxStringView* reason_json,
        PluginxxString*           error_out
    );

    /// 订阅 (带流结束回调; 追加成员, v3)
    ///
    /// 与 [subscribe_broadcast] 只差 `on_end`: 频道被结束时用 reason JSON 回调一次。
    PluginxxSubscription*(PLUGINXX_CALL* subscribe_broadcast_ex)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* broadcast_id,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payload_json, void* ud),
        void(PLUGINXX_CALL* on_end)(const PluginxxStringView* reason_json, void* ud),
        void*                     ud,
        const PluginxxStringView* options_json,
        PluginxxString*           error_out
    );
} PluginxxBroadcastsIface;

/* ==================== 接口表: 能力 (<应用名>.capabilities) ==================== */

#define PLUGINXX_IFACE_CAPABILITIES       PLUGINXX_APP_ID("capabilities")
/// v4: 声明 + 多实现行 (阶段 / 优先级 / 合并), 取代 v3 的"唯一提供者"
/// v5: 表尾追加 `invoke_capability_ex` (带本次调用的超时), 基线与最低版本不变
/// v6: 表尾追加 `unregister_capability_ex` (按作用域注销一条实现行), 基线不变
#define PLUGINXX_IFACE_CAPABILITIES_VERSION     6
#define PLUGINXX_IFACE_CAPABILITIES_MIN_VERSION 4
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_CAPABILITIES_BASELINE_SIZE \
    (offsetof(PluginxxCapabilitiesIface, invoke_capability_ex))

/// 一次能力登记 (插件 → 宿主)
///
/// 两种写法 (`id` 只看前缀, 不反解):
/// 1. 给应用声明过的能力提供实现: 写全名 (`<应用名>.*`), `impls` / `merge` / `phases` 忽略;
/// 2. 声明/实现自己的能力: 写短名 (可含点), 宿主补成 `plugin.<本插件 id>.<名字>`,
///    `impls` 必填 (以一行的 `phases` 说明开放哪些阶段)。
typedef struct PluginxxCapabilitySpec {
    int32_t  version;      ///< 插件填 PLUGINXX_API_VERSION (与宿主一致)
    uint32_t struct_size;  ///< sizeof(PluginxxCapabilitySpec)

    PluginxxStringView id;        ///< 全名或短名 (见上)
    PluginxxStringView owner_tag; ///< 同一实例内区分多个处理器 (可空)

    int32_t  impls;      ///< PLUGINXX_CAPABILITY_IMPLS_* (声明自己的名字时必填)
    int32_t  merge;      ///< PLUGINXX_CAPABILITY_MERGE_* (同上)
    uint32_t phases;     ///< PLUGINXX_CAPABILITY_PHASE_MASK_* (同上)
    int32_t  phase;      ///< 这次登记的阶段: PLUGINXX_CAPABILITY_PHASE_*
    int32_t  priority;   ///< 小者先
    int32_t  sync;       ///< 1 = 只给同步处理器; 0 = 异步 (hook_start)
    int32_t  timeout_ms; ///< 0 = 用声明里的值

    /// 处理器 (二者取一; 同步处理器在返回前完成 `notify`, 异步处理器可稍后完成)
    PluginxxCapabilityStartFunction  run_start;
    PluginxxOperatorCancelFunction   cancel;
    void*                            user_data;
} PluginxxCapabilitySpec;

typedef struct PluginxxCapabilitiesIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_CAPABILITIES_VERSION
    uint32_t struct_size;

    /// 登记 / 声明 (**任意线程**; 宿主投递到 IO 线程执行)
    /// - `spec->id` 为空、阶段未开放、`impls` 自相矛盾时拒绝并写 `error_out`;
    /// - `impls: one` 的 impl 行后登记 (含跨插件) 整体覆盖; `impls: many` 多行共存
    int32_t(PLUGINXX_CALL* register_capability)(
        const PluginxxHost*            host,
        const PluginxxScopeRef*        scope,
        const PluginxxCapabilitySpec*  spec,
        PluginxxString*                error_out
    );

    /// 注销本插件的一条实现行 (**任意线程**; 行不存在返回 [PLUGINXX_RESULT_ERR_NOT_DECLARED])
    int32_t(PLUGINXX_CALL* unregister_capability)(
        const PluginxxHost*       host,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* owner_tag
    );

    /// 调用能力 (**异步; 完成回调恰好一次**)
    /// - `args_json` 为空按 `{}` 处理; `phase` 为 0 表示走默认链 (before → impl → after);
    /// - 真正拒绝 (能力不存在 / 没有可用实现) 返回 NULL 并写 `error_out`, 此时不调用 `cb`
    PluginxxOperatorHandle*(PLUGINXX_CALL* invoke_capability)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* args_json,
        int32_t                   phase,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /// 自省: 一条能力的声明与实现行 (JSON); 未声明返回 [PLUGINXX_RESULT_ERR_NOT_DECLARED]
    int32_t(PLUGINXX_CALL* capability_info)(
        const PluginxxHost*       host,
        const PluginxxStringView* capability_id,
        PluginxxString*           out_json
    );

    /// 自省: 全部能力与实现行数 (JSON)
    int32_t(PLUGINXX_CALL* list_capabilities)(
        const PluginxxHost* host,
        PluginxxString*     out_json
    );

    /// 取消一次异步能力调用 (与 scheduler / tasks 的取消同语义)
    void(PLUGINXX_CALL* op_cancel)(PluginxxOperatorHandle* op);

    /// 调用能力 (带**这次调用**的超时; 追加成员, 老主程序没有它)
    ///
    /// 与 [invoke_capability] 只差一个 `timeout_ms`:
    /// - `> 0`: 这次调用最多等这么久 (到点按取消终结, 完成回调 status = CANCELED);
    /// - `0`: 不设超时 (一直等, 由应用自己收尾);
    /// - `< 0`: 用声明里的值 (等价于 [invoke_capability])。
    ///
    /// 用 `pluginxx::ifaceHas<&PluginxxCapabilitiesIface::invoke_capability_ex>` 判定;
    /// 老主程序没有这个成员时退回 [invoke_capability] (只按声明的超时)。
    PluginxxOperatorHandle*(PLUGINXX_CALL* invoke_capability_ex)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* args_json,
        int32_t                   phase,
        int32_t                   timeout_ms,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /// 注销**指定作用域**上的一条实现行 (追加成员, v6)
    ///
    /// 与 [unregister_capability] 的差别: 那个是本实例的全部作用域, 这个是给定作用域的
    /// 那一行 —— 同一个插件被多个作用域各挂载一次时用它只撤其中一份。
    int32_t(PLUGINXX_CALL* unregister_capability_ex)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   scope,
        const PluginxxStringView* capability_id,
        const PluginxxStringView* owner_tag
    );
} PluginxxCapabilitiesIface;

/* ==================== 接口表: 任务调度 (<应用名>.scheduler) ==================== */

#define PLUGINXX_IFACE_SCHEDULER          PLUGINXX_APP_ID("scheduler")
#define PLUGINXX_IFACE_SCHEDULER_VERSION     1
#define PLUGINXX_IFACE_SCHEDULER_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_SCHEDULER_BASELINE_SIZE sizeof(PluginxxSchedulerIface)

typedef struct PluginxxSchedulerIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_SCHEDULER_VERSION
    uint32_t struct_size;

    int32_t(PLUGINXX_CALL* is_io_thread)(const PluginxxHost* host);
    int32_t(PLUGINXX_CALL*
                post_to_io)(const PluginxxHost* host, void(PLUGINXX_CALL* fn)(void* ud), void* ud);
    PluginxxOperatorHandle*(PLUGINXX_CALL* sleep)(
        const PluginxxHost*      host,
        int64_t                  ms,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );
    void(PLUGINXX_CALL* op_cancel)(PluginxxOperatorHandle* op);

    PluginxxOperatorHandle*(PLUGINXX_CALL* offload)(
        const PluginxxHost* host,
        void*(PLUGINXX_CALL*
                  work)(void* ud, const PluginxxCancelToken* token, PluginxxString* error_out),
        void(PLUGINXX_CALL*
                 done)(void* ud, int32_t status, void* result, const PluginxxStringView* error),
        void*           ud,
        PluginxxString* error_out
    );
} PluginxxSchedulerIface;

/* ==================== 接口表: 协程驱动 (<应用名>.coroutine_runtime) ==================== */

/// 通用协程驱动接口 (与协程库无关)
///
/// 定位: 插件协程与主程序协程在**同一主程序 IO 执行序列**中交错推进的基础设施。
/// 核心只有两类动作:
/// - **driver/pump**: 插件申请主程序异步执行一次有界回调 (push 一个有限步骤);
/// - **wake 合并**: 插件本地有新工作时自行合并重复请求, 再申请下一次 ticket。
///
/// 关键约束 (主程序与插件共同遵守):
/// - `request_driver` **永不内联**回调, 即使调用者就在主程序 IO 线程; 否则 root start /
///   completion / cancel 会形成意外重入, 并失去交错执行的公平性;
/// - 一次 ticket 至多执行一次回调, 且回调只推进一个有限步骤 (不阻塞、不等待);
/// - 主程序不得把插件私有 reactor 的内部等待对象接进自己的执行序列; 插件必须保证
///   每个 driver 都对应"已知的、真实存在的可运行工作" (外部完成回调 / 定时器回调 /
///   已 post 的 continuation), 不得在无工作时持续申请 ticket (那是隐藏轮询);
/// - 重复 wake 由插件适配器自行合并 (同一实例同时只登记一次 ticket); 主程序另做
///   ticket 去重与关闭时取消作为最后防线。
#define PLUGINXX_IFACE_COROUTINE_RUNTIME  PLUGINXX_APP_ID("coroutine_runtime")
#define PLUGINXX_IFACE_COROUTINE_RUNTIME_VERSION     1
#define PLUGINXX_IFACE_COROUTINE_RUNTIME_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_COROUTINE_RUNTIME_BASELINE_SIZE sizeof(PluginxxCoroutineRuntimeIface)

typedef struct PluginxxCoroutineRuntimeIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_COROUTINE_RUNTIME_VERSION
    uint32_t struct_size;

    /// 申请一次驱动请求 (**任意线程可调用**, 非阻塞):
    /// - 成功返回主程序托管的 ticket (主程序只会**异步**调用 drive_once, 每张至多一次);
    /// - 失败返回 NULL 并在 error_out 输出原因 (host->alloc 分配; 实例已关闭/已停用,
    ///   或主程序无可用 IO executor);
    /// - 失败时调用方必须把受影响的操作以失败/取消终结, 不得静默丢弃。
    PluginxxDriver*(PLUGINXX_CALL* request_driver)(
        const PluginxxHost* host,
        PluginxxDriveOnceFn drive_once,
        void*               user_data,
        PluginxxString*     error_out
    );

    /// 取消尚未开始的 ticket (**幂等、非阻塞**; 任意线程可调用):
    /// - 尚未执行的 ticket 之后不再执行回调; 正在执行的回调不会被强行中断,
    ///   它由插件自己的 root 收束协议 (取消/完成) 收尾;
    /// - 取消后票不再持有实例 lease, 因此关闭等待可以继续推进。
    void(PLUGINXX_CALL* cancel_driver)(PluginxxDriver* driver);

    /// 当前线程是否为该实例的 IO 线程 (**仅用于断言与诊断**):
    /// - 只允许插件据此检查自己的用法, **不允许**据此内联执行 driver 回调。
    int32_t(PLUGINXX_CALL* is_io_thread)(const PluginxxHost* host);
} PluginxxCoroutineRuntimeIface;

/* ==================== 接口表: 插件互查 (<应用名>.plugins) ==================== */

#define PLUGINXX_IFACE_PLUGINS            PLUGINXX_APP_ID("plugins")
#define PLUGINXX_IFACE_PLUGINS_VERSION     1
#define PLUGINXX_IFACE_PLUGINS_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_PLUGINS_BASELINE_SIZE sizeof(PluginxxPluginsIface)

typedef struct PluginxxPluginsIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_PLUGINS_VERSION
    uint32_t struct_size;

    int32_t(PLUGINXX_CALL* list_plugins)(const PluginxxHost* host, PluginxxString* out);
    int32_t(PLUGINXX_CALL* get_plugin)(
        const PluginxxHost*       host,
        const PluginxxStringView* name,
        PluginxxString*           out
    );
    int32_t(PLUGINXX_CALL* get_own_info)(const PluginxxHost* host, PluginxxString* out);
} PluginxxPluginsIface;

/* ==================== 接口表: 主程序配置 (<应用名>.config) ==================== */

#define PLUGINXX_IFACE_CONFIG             PLUGINXX_APP_ID("config")
/// v3: 移出主程序业务成员 (工具提示词 / 指定键的工作目录), 改由主程序业务表提供;
/// 本表只保留与业务无关的配置读取
#define PLUGINXX_IFACE_CONFIG_VERSION     3
#define PLUGINXX_IFACE_CONFIG_MIN_VERSION 3
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_CONFIG_BASELINE_SIZE sizeof(PluginxxConfigIface)

typedef struct PluginxxConfigIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_CONFIG_VERSION
    uint32_t struct_size;

    /// 主程序配置的关键字段 JSON (io 线程; host->alloc):
    /// {"dataDir": "...", "projectRoot": "..."(可为空), "platform":
    /// "windows"|"linux"|"macos"|"android"|"ios"}
    int32_t(PLUGINXX_CALL* get_config)(const PluginxxHost* host, PluginxxString* out);
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    int32_t(PLUGINXX_CALL* get_plugin_args)(const PluginxxHost* host, PluginxxString* out);
    /// 本插件配置文件所在目录或文件路径 (yaml `plugins` 条目 config; io 线程;
    /// host->alloc; 未指定返回空串, 空串表示未配置)
    /// - 可指向文件或目录 (由插件自行判断类型并加载)
    /// - 主程序已归一化为绝对路径 (正斜杠, lexically_normal)
    int32_t(PLUGINXX_CALL* get_plugin_config_path)(const PluginxxHost* host, PluginxxString* out);
    /// 读取当前使用的语言 (io 线程; 返回 0 成功, out 填入语言代码如 "en" / "zh-cn", host->alloc
    /// 分配; 默认 "en")
    int32_t(PLUGINXX_CALL* get_language)(const PluginxxHost* host, PluginxxString* out);
    /// 指定使用的语言 (io 线程; 返回 0 成功; 不支持 auto, 为空或 auto 时回退为 "en")
    int32_t(PLUGINXX_CALL* set_language)(
        const PluginxxHost*       host,
        const PluginxxStringView* language
    );
} PluginxxConfigIface;

/* ==================== 接口表: 异步操作取消 (<应用名>.cancel) ==================== */

#define PLUGINXX_IFACE_CANCEL             PLUGINXX_APP_ID("cancel")
/// v3: 移出主程序业务的"指定键是否已取消"查询 (改由主程序业务表提供);
/// 本表只保留通用的一处取消入口
#define PLUGINXX_IFACE_CANCEL_VERSION     3
#define PLUGINXX_IFACE_CANCEL_MIN_VERSION 3
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_CANCEL_BASELINE_SIZE sizeof(PluginxxCancelIface)

typedef struct PluginxxCancelIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_CANCEL_VERSION
    uint32_t struct_size;

    /// 取消一次异步操作句柄 (协作式; 与 scheduler/capabilities/tasks 的取消同语义)
    /// - 异步接口统一在这里取消, 不再每张业务表各留一份
    void(PLUGINXX_CALL* op_cancel)(PluginxxOperatorHandle* op);
} PluginxxCancelIface;

/* ==================== 接口表: JSON 辅助 (<应用名>.json) ==================== */

#define PLUGINXX_IFACE_JSON               PLUGINXX_APP_ID("json")
#define PLUGINXX_IFACE_JSON_VERSION     1
#define PLUGINXX_IFACE_JSON_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_JSON_BASELINE_SIZE sizeof(PluginxxJsonIface)

typedef struct PluginxxJsonIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_JSON_VERSION
    uint32_t struct_size;

    int32_t(PLUGINXX_CALL* json_get_string)(
        const PluginxxHost*       host,
        const PluginxxStringView* json,
        const PluginxxStringView* key,
        PluginxxString*           out
    );
    int32_t(PLUGINXX_CALL* json_escape)(
        const PluginxxHost*       host,
        const PluginxxStringView* s,
        PluginxxString*           out
    );
} PluginxxJsonIface;

/* ==================== 接口表: 日志 (<应用名>.log) ==================== */

#define PLUGINXX_IFACE_LOG                PLUGINXX_APP_ID("log")
#define PLUGINXX_IFACE_LOG_VERSION     1
#define PLUGINXX_IFACE_LOG_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_LOG_BASELINE_SIZE sizeof(PluginxxLogIface)

typedef struct PluginxxLogIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_LOG_VERSION
    uint32_t struct_size;

    void(PLUGINXX_CALL*
             log)(const PluginxxHost* host, int32_t level, const PluginxxStringView* msg);
} PluginxxLogIface;

/* ==================== 接口表: 后台任务 (<应用名>.tasks) ==================== */

#define PLUGINXX_IFACE_TASKS              PLUGINXX_APP_ID("tasks")
#define PLUGINXX_IFACE_TASKS_VERSION     1
#define PLUGINXX_IFACE_TASKS_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_TASKS_BASELINE_SIZE sizeof(PluginxxTasksIface)

typedef struct PluginxxTasksIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_TASKS_VERSION
    uint32_t struct_size;

    /// 注册后台任务 (io 线程约束, 非 io 线程由主程序投递同步等待)。主程序记录
    /// 句柄 (可取消/跟踪完成/持 inflight), 插件协程最终结束时经 *notify
    /// 上报 (恰好一次) → 主程序回收句柄。
    /// - cancel_fn/cancel_ud: 主程序卸载取消时回调 (主程序 io 线程, 协作式):
    ///   唤醒并停止任务; 不可取消可传 NULL
    /// - notify: 【出参】主程序填写的完成通知器 (PluginxxOperatorNotify 值
    ///   拷贝); 插件协程结束 (帧销毁后) 经 notify.done 恰好一次上报 → 主程序
    ///   guard.reset + 回收句柄。以 const 指针形式入参无法回填 —— 主程序只能
    ///   自建一个无法告知插件的 notify, 与本表"插件上报完成"语义矛盾, 必须
    ///   为出参
    /// - notify.done 线程属性与既有 ABI 契约一致: 可从【任意线程】回调
    ///   (主程序 OpCore::onDone 内部原子 CAS + 投递回 io, 线程安全) —— spawn
    ///   协程内若直接调用主程序回调形接口 (invoke_capability_async 等) 或经
    ///   自管线程收尾, 上报可能非 io 线程, 主程序必须按任意线程实现
    /// - 返回主程序托管句柄 (失败返回 NULL 并 *error_out 输出错误, host->alloc)
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_task)(
        const PluginxxHost*            host,
        PluginxxOperatorCancelFunction cancel_fn,
        void*                          cancel_ud,
        PluginxxOperatorNotify*        notify,
        PluginxxString*                error_out
    );
    /// 取消任务 (幂等; 仅限 io 线程调用, 或主程序内部经 ioCallSync 投递后调用)
    /// - 与主程序 detachAll 内部路径一致; 句柄由主程序托管, 跨线程主动取消需经
    ///   scheduler.post_to_io / ioCallSync 回到 io 线程 (与注册类接口线程
    ///   约束一致), 避免 handle->caller 裸指针跨线程反查实例
    void(PLUGINXX_CALL* cancel_task)(PluginxxOperatorHandle* h);
} PluginxxTasksIface;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* PLUGINXX_API_TABLES_H */
