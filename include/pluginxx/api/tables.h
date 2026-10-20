/// pluginxx 通用接口表 (与主程序业务无关的跨边界 C ABI 表)
///
/// 收录: 事件 / 能力 / 任务调度 / 协程驱动 / 插件互查 / 主程序配置 / 会话取消状态 /
/// JSON 辅助 / 日志 / 后台任务。
///
/// 归属判据: **只需要 JSON 载荷与通用资源即可工作** 的表属于通用表 (本头);
/// 需要主程序业务语义 (工具/权限/钩子/会话/模型/提示词/资源/执行图, 以及 client
/// 侧 UI/事件/会话/线路) 的表由主程序定义与实现。
///
/// 契约稳定性: 全部 C 名、结构体名、IID 字符串冻结 (见 abi.h 说明)。
///
/// ════════════════════════════════════════════════════════════════════
/// 接口表的版本与布局规则 (本头每张表都适用)
/// ════════════════════════════════════════════════════════════════════
/// 每张表四项齐全 (缺一不可):
/// - **IID** (`PLUGINXX_IFACE_<名>`): `query_interface` 的键; 换契约 = 换 IID;
/// - **`version`** 字段 + `_VERSION`: 本头描述的完整布局与语义级别; 追加成员或
///   语义变化时 +1; 纯注释变化不动;
/// - **`_MIN_VERSION`**: 本头编写的代码能与之协同工作的最低表版本; 只有"同一 IID
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
/// 本表的当前状态: 全部为 v1 (本次一次性重置, 基线 = 完整布局, 还没有任何追加段)。
#ifndef PLUGINXX_API_TABLES_H
#define PLUGINXX_API_TABLES_H

#include "pluginxx/api/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

/* ==================== 接口表: 事件 (pluginxx.events) ==================== */

#define PLUGINXX_IFACE_EVENTS             "pluginxx.events"
#define PLUGINXX_IFACE_EVENTS_VERSION     1
#define PLUGINXX_IFACE_EVENTS_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_EVENTS_BASELINE_SIZE sizeof(PluginxxEventsIface)

typedef struct PluginxxEventsIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_EVENTS_VERSION
    uint32_t struct_size;

    PluginxxSubscription*(PLUGINXX_CALL* subscribe)(
        const PluginxxHost*       host,
        const PluginxxStringView* topic,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* event_json, void* ud),
        void* ud
    );
    void(PLUGINXX_CALL* unsubscribe)(PluginxxSubscription* sub);
    int32_t(PLUGINXX_CALL* publish)(
        const PluginxxHost*       host,
        const PluginxxStringView* topic,
        const PluginxxStringView* event_json
    );
} PluginxxEventsIface;

/* ==================== 接口表: 能力 (pluginxx.capabilities) ==================== */

#define PLUGINXX_IFACE_CAPABILITIES             "pluginxx.capabilities"
#define PLUGINXX_IFACE_CAPABILITIES_VERSION     1
#define PLUGINXX_IFACE_CAPABILITIES_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_CAPABILITIES_BASELINE_SIZE sizeof(PluginxxCapabilitiesIface)

/// 成员顺序即契约 (含历史上插在中间的 register_capability_ex): 重排成员不会改变
/// 结构体大小, `version` 与 `struct_size` 都拦不住旧调用方的静默错调 —— 禁止再动,
/// 改签名/顺序请换 IID。
typedef struct PluginxxCapabilitiesIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_CAPABILITIES_VERSION
    uint32_t struct_size;

    int32_t(PLUGINXX_CALL* register_capability)(
        const PluginxxHost*       host,
        const PluginxxStringView* capability
    );
    int32_t(PLUGINXX_CALL* register_capability_ex)(
        const PluginxxHost*             host,
        const PluginxxStringView*       capability,
        PluginxxCapabilityStartFunction start,
        PluginxxOperatorCancelFunction  cancel,
        void*                           ctx
    );
    int32_t(PLUGINXX_CALL* unregister_capability)(
        const PluginxxHost*       host,
        const PluginxxStringView* capability
    );
    int32_t(PLUGINXX_CALL* has_capability)(
        const PluginxxHost*       host,
        const PluginxxStringView* capability
    );

    PluginxxOperatorHandle*(PLUGINXX_CALL* invoke_capability_async)(
        const PluginxxHost*       host,
        const PluginxxStringView* capability,
        const PluginxxStringView* method,
        const PluginxxStringView* args_json,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    void(PLUGINXX_CALL* op_cancel)(PluginxxOperatorHandle* op);
} PluginxxCapabilitiesIface;

/* ==================== 接口表: 任务调度 (pluginxx.scheduler) ==================== */

#define PLUGINXX_IFACE_SCHEDULER             "pluginxx.scheduler"
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

/* ==================== 接口表: 协程驱动 (pluginxx.coroutine_runtime) ==================== */

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
#define PLUGINXX_IFACE_COROUTINE_RUNTIME             "pluginxx.coroutine_runtime"
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

/* ==================== 接口表: 插件互查 (pluginxx.plugins) ==================== */

#define PLUGINXX_IFACE_PLUGINS             "pluginxx.plugins"
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

/* ==================== 接口表: 主程序配置 (pluginxx.config) ==================== */

#define PLUGINXX_IFACE_CONFIG             "pluginxx.config"
#define PLUGINXX_IFACE_CONFIG_VERSION     1
#define PLUGINXX_IFACE_CONFIG_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_CONFIG_BASELINE_SIZE sizeof(PluginxxConfigIface)

typedef struct PluginxxConfigIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_CONFIG_VERSION
    uint32_t struct_size;

    /// 主程序 AgentConfig 关键字段 JSON (io 线程; host->alloc):
    /// {"dataDir": "...", "projectRoot": "..."(可为空), "platform":
    /// "windows"|"linux"|"macos"|"android"|"ios"}
    int32_t(PLUGINXX_CALL* get_config)(const PluginxxHost* host, PluginxxString* out);
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    int32_t(PLUGINXX_CALL* get_plugin_args)(const PluginxxHost* host, PluginxxString* out);
    /// 主程序 toolPrompt 配置 (io 线程; host->alloc):
    /// {"depict": "...", "args": {"参数名": "参数说明", ...}}
    int32_t(PLUGINXX_CALL* get_tool_prompt)(
        const PluginxxHost*       host,
        const PluginxxStringView* tool_name,
        PluginxxString*           out
    );
    /// 指定会话生效的工作目录 (io 线程; host->alloc; 失败/未加载返回空串):
    /// - session_id 非空: worktree 绑定优先, 依次回退会话覆写 / AgentConfig
    /// - session_id 为空: 返回解析后的默认会话工作目录
    int32_t(PLUGINXX_CALL* get_session_work_dir)(
        const PluginxxHost*       host,
        const PluginxxStringView* session_id,
        PluginxxString*           out
    );
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

/* ==================== 接口表: 会话取消状态 (pluginxx.cancel) ==================== */

#define PLUGINXX_IFACE_CANCEL             "pluginxx.cancel"
#define PLUGINXX_IFACE_CANCEL_VERSION     1
#define PLUGINXX_IFACE_CANCEL_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_CANCEL_BASELINE_SIZE sizeof(PluginxxCancelIface)

typedef struct PluginxxCancelIface {
    int32_t  version; ///< 主程序填 PLUGINXX_IFACE_CANCEL_VERSION
    uint32_t struct_size;
    /// 查询会话当前轮次是否已取消 (advisory 定位; 权威通知始终是 cancel 回调)

    int32_t(PLUGINXX_CALL* is_cancelled)(
        const PluginxxHost*       host,
        const PluginxxStringView* session_id
    );
} PluginxxCancelIface;

/* ==================== 接口表: JSON 辅助 (pluginxx.json) ==================== */

#define PLUGINXX_IFACE_JSON             "pluginxx.json"
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

/* ==================== 接口表: 日志 (pluginxx.log) ==================== */

#define PLUGINXX_IFACE_LOG             "pluginxx.log"
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

/* ==================== 接口表: 后台任务 (pluginxx.tasks) ==================== */

#define PLUGINXX_IFACE_TASKS             "pluginxx.tasks"
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
