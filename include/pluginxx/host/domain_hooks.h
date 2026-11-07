/// pluginxx 主程序业务数据入口 (通用表实现的取数入口)
///
/// 定位: 通用表 (**与主程序业务无关**的表) 的实现位于 cxx_pluginxx, 但其中少数入口
/// 需要主程序数据 (配置内容 / 语言 / 插件清单 / 工作线程池 / 事件总线)。这些数据一律
/// 经本接口取, 因此 pluginxx 源码不包含任何主程序头、不依赖任何主程序类型。
/// 主程序业务语义 (工具 / 提示词 / 会话 / 图 ...) 一律走主程序自己的业务表,
/// 不进入本接口。
///
/// 主程序侧用法: 主程序管理器实现本接口, 并在任何通用表入口可能被调用之前
/// 经 [PluginHostCore::setDomainHooks] 注入 (通常在管理器构造函数体内完成)。
///
/// 线程约定: 下列方法都在主程序 IO 线程被调用 (通用表入口先投递到 IO 线程),
/// 实现方无需额外加锁; [postToWorkerThread] 例外, 它把工作投递到工作线程池。
#pragma once

#include "pluginxx/api/abi.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pluginxx {

/// 主程序业务数据入口 (纯虚接口部分为必需项; 带默认实现的为可选项)
class DomainHooks {
public:

    virtual ~DomainHooks() = default;

    // ==================== 广播 / 能力共用 ====================

    /// 补齐广播 id 的命名空间 (订阅与发布都经它, 保证两侧 id 一致)
    /// - 主程序可自行规定前缀 (例如插件短名补成 `plugin.<本插件 id>.<名字>`)
    virtual std::string qualifyEventTopic(std::string_view topic) {
        return std::string{topic};
    }

    /// 实例名 → 插件 id (默认相同; 主程序有"实例名前缀"时覆盖它)
    /// - 用途: 插件用短名声明/实现自己的能力时, 宿主拼出 `plugin.<插件 id>.<名字>` 的全名
    virtual std::string pluginId(std::string_view instanceName) {
        return std::string{instanceName};
    }

    // ==================== 调度表 ====================

    /// 把一段阻塞工作投递到主程序工作线程池
    /// - `return` false 表示主程序不可用 (offload 以失败终结, 不静默丢工作)
    virtual bool postToWorkerThread(std::function<void()> fn) = 0;

    // ==================== config 表 ====================

    /// 主程序配置 JSON (`get_config` 的业务部分, 如 dataDir / projectRoot / platform)
    virtual std::string configJson() = 0;

    /// 当前语言代码 (如 "en" / "zh-cn"); 空串由调用方回退为 "en"
    virtual std::string language() = 0;

    /// 指定语言 (返回值忽略: 只读查询失败与写入失败在 ABI 上都表现为空结果)
    virtual void setLanguage(std::string_view lang) = 0;

    // ==================== plugins 表 ====================

    /// 全部插件 JSON 数组 (`list_plugins`)
    virtual std::string pluginsJson() = 0;

    /// 单个插件 JSON 对象 (`get_plugin` / `get_own_info`; 不存在返回空串)
    virtual std::string pluginJson(std::string_view name) = 0;
};

} // namespace pluginxx
