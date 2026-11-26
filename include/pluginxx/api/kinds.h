/// 插件形态（kind）接口表：让插件登记"本插件提供 kind=X"
///
/// 主程序框架自己只实现**动态库插件**与它自带的解释执行为形态（例如脚本运行器）；其它形态
/// （以后的 Python 等）由插件登记：声明"本插件提供 kind=X"，并交出一对能力（load / unload）。
/// 框架在扫描、判定与加载 `kind: X` 的插件时都查这张表：
///
/// - **判定**：没有登记的 `kind` 判为"当前跑不了"（用户会看到"先装运行器插件"）；
/// - **每个形态有自己的接口代次对**（`interfaceVersion` / `minInterfaceVersion`）：清单的
///   `api_version` 按它判（`>= 下限`）；会调 C 接口表的形态一律用框架的 C 接口那一对；
/// - **校验**：可选的 `validateCapability` —— 加载前（以及安装预检时）先问它
///   "这个目录能不能作为这个形态跑"，它比框架更清楚这种插件长什么样；
/// - **加载 / 卸载**：框架调登记时给的能力，`{"ok":true}` 表示接手成功。
///
/// 登记的形态跟着插件生命周期撤销（停用 / 卸载 / 关闭插件框架），不写磁盘。
/// 表本身由框架提供；注册表实现在 `pluginxx/host/kind_registry.h`。
///
/// IID 与框架的通用表同一套拼法（`PLUGINXX_APP_ID("kind")` → `<应用名>.kind`），
/// 成员是"登记 / 撤销 / 列表"三个异步操作。
#ifndef PLUGINXX_API_KINDS_H
#define PLUGINXX_API_KINDS_H

#include "pluginxx/api/abi.h"
#include "pluginxx/api/app.h"
#include "pluginxx/api/result.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

#define PLUGINXX_IFACE_KIND PLUGINXX_APP_ID("kind")
#define PLUGINXX_IFACE_KIND_VERSION 2
#define PLUGINXX_IFACE_KIND_MIN_VERSION 2
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为第一个追加成员的偏移) */
#define PLUGINXX_IFACE_KIND_BASELINE_SIZE sizeof(PluginxxKindIface)

typedef struct PluginxxKindIface {
    int32_t  version;     ///< 框架填 PLUGINXX_IFACE_KIND_VERSION
    uint32_t struct_size;

    /// 登记一种插件形态（本插件成为它的运行器）
    ///
    /// `spec_json`：
    /// ```json
    /// {"kind":"python","requiresPluginAbi":false,
    ///  "interfaceVersion":3,"minInterfaceVersion":1,
    ///  "loadCapability":"plugin.my_runtime.kind_load",
    ///  "unloadCapability":"plugin.my_runtime.kind_unload",
    ///  "validateCapability":"plugin.my_runtime.kind_validate",
    ///  "describe":"Python runner",
    ///  "overrideBuiltin":false}
    /// ```
    /// - `kind`：清单里写的形态名（小写字母、数字、`_` `-` `.`，1~32 字）；
    /// - `requiresPluginAbi`：该形态的插件是否按 C ABI 编译（缺省 false）。写 true 表示
    ///   它们会调通用接口表，接口代次对固定用框架的 C 接口那一对（写了自己也不采信）；
    /// - `interfaceVersion` / `minInterfaceVersion`：解释执行的形态自己的**接口代次对**
    ///   （当前代次 / 兼容下限）。清单 `api_version` 的判定是 `>= minInterfaceVersion`；
    ///   插件读到的是 `interfaceVersion`。只增不改时只抬当前代次，破坏性变更才抬下限；
    ///   都不写 = 不判；
    /// - `loadCapability` / `unloadCapability`：**必须写在本插件的命名空间**
    ///   （`plugin.<插件id>.`）下；缺 `loadCapability` 会被拒绝；
    /// - `validateCapability`（可选，同样要在本插件命名空间下）：加载一个该形态的插件之前
    ///   先问它一次，载荷 `{"action":"validate","kind","id","path","version"}`，回答
    ///   `{"ok":true}` = 可以跑，`{"ok":false,"error":"..."}` = 拒绝（理由直接给用户）；
    /// - `overrideBuiltin`：写 true 表示**接管框架自带的形态**（只允许解释执行的形态，
    ///   例如脚本运行器）；接管后撤销即回到框架自带的实现。
    ///
    /// 完成时: 成功无结果正文; 失败正文是原因（`arg` / `state` / `taken` / `namespace`）。
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_kind)(
        const PluginxxHost*       host,
        const PluginxxStringView* spec_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// 撤销本插件登记的形态（不是自己登记的按 `not_owner` 失败）
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_kind)(
        const PluginxxHost*       host,
        const PluginxxStringView* kind,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// 列出全部形态（框架自带的 + 插件登记），结果 JSON 数组：
    /// `[{"kind","builtin","requiresPluginAbi","interfaceVersion","minInterfaceVersion",
    ///    "provider","providerId","loadCapability","unloadCapability","validateCapability",
    ///    "describe","overridden","available"}]`
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_kinds)(
        const PluginxxHost*       host,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );
} PluginxxKindIface;

/// 运行器能力的**载荷与回答**（`loadCapability` / `unloadCapability` 收发的 JSON）
///
/// 请求（框架 → 运行器）：
/// ```json
/// {"action":"load","kind":"python","id":"demo","path":"<插件目录>",
///  "instance":"python:demo","version":"1.0.0"}
/// ```
/// `action` 取 `load` / `unload`；`instance` 是建议的实例名（`<kind>:<插件id>`），运行器
/// 可以只用它做日志与命名空间。回答 `{"ok":true}` 表示接手成功；`{"ok":false,
/// "error":"..."}` 或能力失败表示这次加载 / 卸载失败。运行器**不要在能力里阻塞**：
/// 框架会等这次回答（有超时），长任务请自己排队。

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* PLUGINXX_API_KINDS_H */
