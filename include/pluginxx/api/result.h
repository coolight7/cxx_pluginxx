#pragma once

// 返回码：插件框架这一侧只有这一份表
//
// 适用范围：`pluginxx` 的表入口（广播 / 能力 / 生命周期 / 内部）。
// 网络段（-100 ~ -199）不在这里：它属于 `utilxx/net_error.h`（只有实现网络的那一层能产生它），
// 需要名称的调用方（主程序、插件包）两个头都 include。
//
// 约定：
// - **每个数字必须有返回点**：没有生产者的码不加；
// - **数字只到 C / FFI 边界**：JSON 与上层接口里一律用名称（`nameOf()`），
//   Dart 侧通过导出的 `musicxx_extern_plugin_result_name()` 查，不另抄一份表；
// - **预留段不定义码**：能力（-300 ~ -399）与 UI（-400 ~ -499）先空着；
// - 分段与含义见包内 `docs/plugin-api.md` §11.4。

#include <stdint.h>

/* ==================== 成功 ==================== */

#define PLUGINXX_RESULT_OK 0

/* ==================== 通用段 (-1 ~ -99) ==================== */
// 任何表、任何入口都可能返回

#define PLUGINXX_RESULT_ERR_ARG (-1)          ///< 参数非法（缺字段 / 类型不对 / 值超范围）
#define PLUGINXX_RESULT_ERR_STATE (-2)        ///< 当前状态不允许（没启动 / 已停止 / 已经做过）
#define PLUGINXX_RESULT_ERR_JSON (-3)         ///< 参数不是合法 JSON
#define PLUGINXX_RESULT_ERR_NOT_FOUND (-4)    ///< 目标不存在（id / 频道 / 请求 / 服务）
#define PLUGINXX_RESULT_ERR_TIMEOUT (-5)      ///< 等对端（主程序线程 / 插件）超时
#define PLUGINXX_RESULT_ERR_PERMISSION (-6)   ///< 没有权限（不是属主 / 系统频道 / 路径越界）
#define PLUGINXX_RESULT_ERR_BUSY (-7)         ///< 排队已满（连接 / 请求 / 服务上限）
#define PLUGINXX_RESULT_ERR_NO_MEMORY (-8)    ///< 内存不足

/* ==================== 广播段 (-200 ~ -299) ==================== */

#define PLUGINXX_RESULT_ERR_NOT_DECLARED (-200)  ///< 频道没有声明
#define PLUGINXX_RESULT_ERR_NO_VALUE (-201)      ///< 没有当前值（`cache: none` 或还没写过）
#define PLUGINXX_RESULT_ERR_ENDED (-202)         ///< 流已结束（发布 / 读取 / 订阅都被拒）
#define PLUGINXX_RESULT_ERR_NOT_OWNER (-203)     ///< 不是这条频道的属主

/* ==================== 生命周期 / 内部段 ==================== */

#define PLUGINXX_RESULT_ERR_NOT_READY (-500)  ///< 还没准备好（主程序未启动 / 加载中）
#define PLUGINXX_RESULT_ERR_INTERNAL (-999)   ///< 实现内部错误（不该发生）

/* ==================== 自定义段（只约定区间，不定义码） ==================== */
//
// -5000 ~ -9999   应用自定义（不同应用各有一套）
// -10000 ~ -19999 插件自定义（各插件各有一套）
//
// 用自有码报错时，主程序按区间给出类别（`app_custom` / `plugin_custom`），名称由属主自己解释：
// `nameOf()` 对这两段返回 nullptr，`categoryOf()` 给类别。

#ifdef __cplusplus
namespace pluginxx {
namespace result {

/// 全部分段的名称（不属于已定义分段时返回 nullptr）
///
/// `inline`：头文件即用，各库/插件各编译一份字面量，值相同。
inline const char* nameOf(int32_t code) {
  switch (code) {
    case PLUGINXX_RESULT_OK: return "ok";

    case PLUGINXX_RESULT_ERR_ARG: return "invalid_argument";
    case PLUGINXX_RESULT_ERR_STATE: return "invalid_state";
    case PLUGINXX_RESULT_ERR_JSON: return "invalid_json";
    case PLUGINXX_RESULT_ERR_NOT_FOUND: return "not_found";
    case PLUGINXX_RESULT_ERR_TIMEOUT: return "timeout";
    case PLUGINXX_RESULT_ERR_PERMISSION: return "permission_denied";
    case PLUGINXX_RESULT_ERR_BUSY: return "busy";
    case PLUGINXX_RESULT_ERR_NO_MEMORY: return "out_of_memory";

    case PLUGINXX_RESULT_ERR_NOT_DECLARED: return "not_declared";
    case PLUGINXX_RESULT_ERR_NO_VALUE: return "no_value";
    case PLUGINXX_RESULT_ERR_ENDED: return "ended";
    case PLUGINXX_RESULT_ERR_NOT_OWNER: return "not_owner";

    case PLUGINXX_RESULT_ERR_NOT_READY: return "not_ready";
    case PLUGINXX_RESULT_ERR_INTERNAL: return "internal";

    default: return nullptr;
  }
}

/// 这个码属于哪一段
///
/// 返回值固定为：`ok` / `general` / `broadcast` / `capability` / `ui` / `lifecycle` /
/// `internal` / `app_custom` / `plugin_custom` / `unknown`。
/// 能力与 UI 两段目前是预留（没有定义码），这里先按区间给类别，以后加码不用改这里。
inline const char* categoryOf(int32_t code) {
  if (code == PLUGINXX_RESULT_OK) {
    return "ok";
  }
  if (code <= -1 && code >= -99) {
    return "general";
  }
  if (code <= -100 && code >= -199) {
    return "net";
  }
  if (code <= -200 && code >= -299) {
    return "broadcast";
  }
  if (code <= -300 && code >= -399) {
    return "capability";
  }
  if (code <= -400 && code >= -499) {
    return "ui";
  }
  if (code <= -500 && code >= -599) {
    return "lifecycle";
  }
  if (code <= -900 && code >= -999) {
    return "internal";
  }
  if (code <= -5000 && code >= -9999) {
    return "app_custom";
  }
  if (code <= -10000 && code >= -19999) {
    return "plugin_custom";
  }
  return "unknown";
}

}  // namespace result
}  // namespace pluginxx

#endif  // __cplusplus
