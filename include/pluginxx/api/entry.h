/// pluginxx 主程序插件入口符号名 (由主程序提供, 插件框架不硬编码任何主程序专名)
///
/// 背景: 插件以动态库分发时, 主程序按一组约定符号名在库里查找入口函数
/// (`get_info` / `create` / `stop` / `destroy`)。这组符号名属于**主程序的命名空间**,
/// 不属于插件框架:
/// - 形如 `<应用名>_<端>_plugin_<入口>` (本应用是 `agentxx_agent_plugin_*` /
///   `agentxx_client_plugin_*`, 两端各一套)。
///
/// 因此插件框架只定义"主程序怎么把符号名交进来"这一扩展点:
/// - 主程序侧: 覆写 `pluginxx::PluginHostLifecycle::entrySymbols()` 返回自己的符号名;
/// - 插件侧: 用带符号前缀的导出宏生成入口 (见 `pluginxx/kit/kit.h` 的
///   `PLUGINXX_EXPORT_PLUGIN` / `PLUGINXX_EXPORT_PLUGIN_LIFECYCLE`)。
///
/// 未提供必需符号名 (默认空) 时加载直接失败并给出明确原因, 不会去猜主程序专名。
#ifndef PLUGINXX_API_ENTRY_H
#define PLUGINXX_API_ENTRY_H

#include <string_view>

namespace pluginxx {

/// 主程序插件入口符号名集合 (值必须与插件侧导出宏使用的前缀一致)
struct PluginEntrySymbols {
    /// 可空: 该入口不存在时跳过元信息校验 (名字回落为库文件名)
    std::string_view getInfo;

    /// 必需: 实例创建入口 (构造 + 注册; 缺失即加载失败)
    std::string_view create;

    /// 必需: 停用入口 (撤销注册与自管资源)
    std::string_view stop;

    /// 必需: 销毁入口 (释放本地对象; 完成后主程序才 dlclose)
    std::string_view destroy;

    /// 三个必需入口是否齐全 (create / stop / destroy)
    bool valid() const noexcept {
        return !create.empty() && !stop.empty() && !destroy.empty();
    }

    /// 缺哪个入口 (用于给出"照什么改"的错误文本; 齐全时返回空视图)
    std::string_view firstMissing() const noexcept {
        if (create.empty()) {
            return "create";
        }
        if (stop.empty()) {
            return "stop";
        }
        if (destroy.empty()) {
            return "destroy";
        }
        return {};
    }
};

} // namespace pluginxx

#endif /* PLUGINXX_API_ENTRY_H */
