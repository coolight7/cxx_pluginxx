/// pluginxx 宿主插件入口符号名 (由宿主提供, 内核不硬编码任何宿主专名)
///
/// 背景: 插件以动态库分发时, 宿主按一组约定符号名在库里查找入口函数
/// (`get_info` / `create` / `start` / `stop`; `destroy` 由实例类给出, 见
/// `pluginxx/runtime/instance_base.h` 的 `pluginDestroySymbol`)。这组符号名属于
/// **宿主的命名空间**, 不属于框架内核:
/// - agentxx 使用 `agentxx_plugin_agent_*` / `agentxx_plugin_client_*`;
/// - musicxx 使用 `musicxx_plugin_*`。
///
/// 因此内核只定义"宿主怎么把符号名交进来"这一接缝:
/// - 宿主侧: 覆写 `pluginxx::PluginHostLifecycle::entrySymbols()` 返回自己的符号名;
/// - 插件侧: 用带符号前缀的导出宏生成入口 (见 `pluginxx/kit/kit.h` 的
///   `PLUGINXX_EXPORT_PLUGIN` / `PLUGINXX_EXPORT_PLUGIN_LIFECYCLE`)。
///
/// 未提供符号名 (默认空) 时装载直接失败并给出明确原因, 不会去猜宿主专名。
#ifndef PLUGINXX_API_ENTRY_H
#define PLUGINXX_API_ENTRY_H

#include <string_view>

namespace pluginxx {

/// 宿主插件入口符号名集合 (值必须与插件侧导出宏使用的前缀一致)
struct PluginEntrySymbols {
    /// 可空: 该入口不存在时跳过元信息校验 (与"可选符号"同语义)
    std::string_view getInfo;

    /// 必需: 实例创建入口 (缺失即装载失败)
    std::string_view create;

    /// 必需: start 事务入口 (缺失即装载失败; 现行契约要求 start/stop 成对)
    std::string_view start;

    /// 必需: stop 事务入口 (缺失即装载失败)
    std::string_view stop;

    /// create 入口是否已提供 (最基本的要求)
    bool valid() const noexcept {
        return !create.empty();
    }

    /// start/stop 是否成对提供 (两者都非空或都为空)
    bool lifecyclePaired() const noexcept {
        return start.empty() == stop.empty();
    }
};

} // namespace pluginxx

#endif /* PLUGINXX_API_ENTRY_H */
