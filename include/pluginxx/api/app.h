#pragma once

/// 应用名与应用命名空间下的标识 (接口表 IID、官方能力 / 广播 id 的前缀)
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

/// 应用命名空间下的标识: `PLUGINXX_APP_ID("capabilities")` → `"your_app.capabilities"`
///
/// - 通用接口表 IID 用它拼出 (`PLUGINXX_IFACE_CAPABILITIES` 等), 源码里不再有具体前缀的字面量;
/// - 应用名与表名都可以含点 (`com.foo` + `capabilities` → `com.foo.capabilities`),
///   **不做格式检查**: IID 只做整串比较, 官方标识只做前缀比较, 都不需要按点切分。
#define PLUGINXX_APP_ID(name) PLUGINXX_APP_NAME "." name

#ifdef __cplusplus
#include <string_view>

namespace pluginxx {

/// 官方标识前缀 (`<应用名>.`): 官方能力 / 广播 id 都以它开头
inline std::string_view appIdPrefix() {
    return PLUGINXX_APP_NAME ".";
}

} // namespace pluginxx
#endif /* __cplusplus */
