#pragma once

/// 应用名与应用命名空间下的标识 (接口表 IID、官方能力 / 广播 id 的前缀)
///
/// 两套拼法, 分工明确:
/// - 接口表 IID: 下划线, [PLUGINXX_TABLE_ID] (`<应用名>_plugin_log` / `<应用名>_agent_tools`);
/// - 官方能力 / 广播 / 功能埋点 id: 点分, [PLUGINXX_APP_ID] (`<应用名>.agent.tools`)。
///
/// - 应用名由**构建方**给出: CMake 缓存变量 `PLUGINXX_APP_NAME` 以 PUBLIC 编译定义
///   下发给本库的所有使用方, 主程序与插件因此看到同一个值;
/// - 默认值 `pluginxx` 表示"库自己就是应用": 本库自身的测试与工具不配置也能编、能跑;
///   主程序**必须**覆盖它 (例如 `-DPLUGINXX_APP_NAME=your_app`);
/// - 运行期还有一次覆盖机会: 主程序拿到预编译库、改不了构建宏时, 在管理器构造后调用
///   `pluginxx::PluginHostCore::setOfficialPrefix()` 设置官方标识前缀。
///
/// 命名空间分工:
/// - `<应用名>.*` = 接口表 (通用表与业务表) 与应用自己的标识;
/// - `plugin.<插件 id>.*` = 插件自己的标识 (不随应用名变)。
#ifndef PLUGINXX_APP_NAME
#define PLUGINXX_APP_NAME "pluginxx"
#endif

/// 应用命名空间下的**点分**标识: `PLUGINXX_APP_ID("agent.tools")` → `"your_app.agent.tools"`
///
/// 用在需要点分命名空间的地方: 官方能力 id、广播频道 id、功能埋点 id
/// (`<应用名>.<名字>`)。**接口表 IID 不用它** (表 IID 一律下划线, 见
/// [PLUGINXX_TABLE_ID])。
///
/// - 应用名与名字都可以含点 (`com.foo` + `agent.tools` → `com.foo.agent.tools`),
///   **不做格式检查**: id 只做整串比较, 官方标识只做前缀比较, 都不需要按点切分。
#define PLUGINXX_APP_ID(name) PLUGINXX_APP_NAME "." name

/// 应用命名空间下的**下划线**标识 (接口表 IID):
/// `PLUGINXX_TABLE_ID("plugin_log")` → `"your_app_plugin_log"`
///
/// 两段划分 (见 docs 的框架约定):
/// - 框架给的通用表: 名字以 `plugin_` 开头 (`plugin_log` / `plugin_capabilities` …),
///   于是 IID 是 `<应用名>_plugin_*`;
/// - 应用自己的业务表: 名字带端前缀 (`agent_tools` / `client_ui` …),
///   于是 IID 是 `<应用名>_<端>_*`。
///
/// 源码里因此没有具体前缀的字面量: 换应用名只改构建配置。
#define PLUGINXX_TABLE_ID(name) PLUGINXX_APP_NAME "_" name

#ifdef __cplusplus
#include <string_view>

namespace pluginxx {

/// 官方标识前缀 (`<应用名>.`): 官方能力 / 广播 id 都以它开头
inline std::string_view appIdPrefix() {
    return PLUGINXX_APP_NAME ".";
}

/// 通用表 IID 前缀 (`<应用名>_`): `PLUGINXX_TABLE_ID` 拼出来的 IID 都以它开头
inline std::string_view appTableIdPrefix() {
    return PLUGINXX_APP_NAME "_";
}

} // namespace pluginxx
#endif /* __cplusplus */
