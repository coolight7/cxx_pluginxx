/// pluginxx 插件清单与名称推导实现 (见 pluginxx/host/manifest.h)
///
/// 提取到插件框架的原因: 主程序多种插件框架共用同一套清单解析/
/// 名称推导/入口路径解析, 放在一处可避免两侧行为差异。
#include "pluginxx/host/manifest.h"

#include "pluginxx/api/abi.h"
#include "utilxx_base/json.h"
#include "yaml-cpp/yaml.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace pluginxx {

std::string_view pluginStringView2std(PluginxxStringView str) {
    return std::string_view{str.data, static_cast<size_t>(str.size)};
}

namespace {

/// YAML 值转 JSON (资源声明的附加字段以 JSON 文本交给主程序)
///
/// - 序列/映射递归转换; 标量按整数 → 布尔 → 浮点 → 字符串的顺序尝试;
/// - **标量必须用圆括号构造**: `utilxx_base::Json` 有 initializer_list 构造函数,
///   花括号初始化会优先匹配它, 把标量包成单元素数组 (`Json{true}` → `[true]`);
/// - 先判标量再判序列: yaml-cpp 对单标量节点也可能按"长度 1 的序列"迭代,
///   顺序反过来会把每个标量都包成数组。
utilxx_base::Json yamlToJson(const YAML::Node& node) {
    if (!node.IsDefined() || node.IsNull()) {
        return utilxx_base::Json{};
    }
    if (node.IsScalar()) {
        std::string text = node.as<std::string>();
        // 整数 (允许前导正负号; 不用 stoll 避免抛异常)
        {
            const char* first = text.data();
            const char* last  = text.data() + text.size();
            long long   value = 0;
            if (auto rc = std::from_chars(first, last, value);
                rc.ec == std::errc{} && rc.ptr == last) {
                return utilxx_base::Json(value);
            }
        }
        if (text == "true" || text == "True") {
            return utilxx_base::Json(true);
        }
        if (text == "false" || text == "False") {
            return utilxx_base::Json(false);
        }
        {
            const char* first = text.data();
            const char* last  = text.data() + text.size();
            double      value = 0;
            if (auto rc = std::from_chars(first, last, value);
                rc.ec == std::errc{} && rc.ptr == last) {
                return utilxx_base::Json(value);
            }
        }
        return utilxx_base::Json(std::move(text));
    }
    if (node.IsSequence()) {
        utilxx_base::Json arr = utilxx_base::Json::array();
        for (const auto& item : node) {
            arr.push_back(yamlToJson(item));
        }
        return arr;
    }
    if (node.IsMap()) {
        utilxx_base::Json obj = utilxx_base::Json::object();
        for (const auto& kv : node) {
            obj[kv.first.as<std::string>()] = yamlToJson(kv.second);
        }
        return obj;
    }
    return utilxx_base::Json{};
}

/// 提供者登记位 (函数内静态: 与任何静态初始化顺序无关)
BuiltinPluginProvider& providerSlot() noexcept {
    static BuiltinPluginProvider provider{};
    return provider;
}
} // namespace

void setBuiltinPluginProvider(BuiltinPluginProvider provider) noexcept {
    providerSlot() = provider;
}

BuiltinPluginProvider builtinPluginProvider() noexcept {
    return providerSlot();
}

const PluginxxBuiltinInfo* findBuiltinPlugin(std::string_view name) {
    auto fn = providerSlot().plugins;
    if (!fn) {
        return nullptr;
    }
    uint64_t    count = 0;
    const auto* list  = fn(&count);
    if (!list) {
        return nullptr;
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (pluginStringView2std(list[i].name) == name) {
            return &list[i];
        }
    }
    return nullptr;
}

const PluginxxBuiltinManifest* findBuiltinManifest(std::string_view name) {
    auto fn = providerSlot().manifests;
    if (!fn) {
        return nullptr;
    }
    uint64_t    count = 0;
    const auto* list  = fn(&count);
    if (!list) {
        return nullptr;
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (pluginStringView2std(list[i].name) == name) {
            return &list[i];
        }
    }
    return nullptr;
}

std::string pluginNameFromPath(const std::string& path) {
    auto base = std::filesystem::path(path).filename().string();
    // 去扩展名及其后版本号 (rfind: 兼容文件名内含扩展名片段, 如
    // my.plugin.so → my.plugin; libfoo.so.1.2 → libfoo)
    bool removedExt = false;
    for (const char* ext : {".dylib", ".so", ".dll"}) {
        auto pos = base.rfind(ext);
        if (pos != std::string::npos && pos > 0) {
            base.erase(pos);
            removedExt = true;
            break;
        }
    }
    // 去 lib 前缀 (仅当剥离过扩展名: 无扩展名的库/目录保持原名, 防误剥)
    if (removedExt && base.size() > 3 && base.compare(0, 3, "lib") == 0) {
        base.erase(0, 3);
    }
    return base;
}

namespace {
/// 共享的 Node 解析逻辑 (文件与字符串共用)
bool parseManifestNode(
    const YAML::Node&            node,
    const std::filesystem::path& baseDir,
    const std::string&           logHint,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends,
    PluginManifestResources*     resources,
    PluginManifestInterfaces*    interfaces
) {
    try {
        name  = node["name"] ? node["name"].as<std::string>() : std::string{};
        entry = node["entry"] ? node["entry"].as<std::string>() : std::string{};
        if (node["depends"] && node["depends"].IsSequence()) {
            for (const auto& d : node["depends"]) {
                if (d.IsScalar()) {
                    depends.push_back(d.as<std::string>());
                }
            }
        }
        if (node["optional_depends"] && node["optional_depends"].IsSequence()) {
            for (const auto& d : node["optional_depends"]) {
                if (d.IsScalar()) {
                    optionalDepends.push_back(d.as<std::string>());
                }
            }
        }
        // ---- 资源声明段 ----
        // 框架只做"解析 + 相对路径归一", 资源种类 (type) 与字段语义交给主程序:
        // - 规范写法: `resources:` 列表, 每项 {type, path?, args?}
        // - 兼容旧写法: 顶层 `skill:` / `memory:` / `mcp:` 段 (元素可为标量路径
        //   或对象), 按同名 type 逐条映射成条目
        // 段缺失/非法项跳过并告警, 不影响 manifest 合法性 (声明是可选增强)
        if (resources) {
            *resources = PluginManifestResources{};
            // 绝对路径原样保留; 相对路径按插件目录拼接并规范化
            // baseDir 为空时 (内置内嵌清单) 保持原样
            auto resolveRel = [&baseDir](const std::string& p) -> std::string {
                if (p.empty()) {
                    return {};
                }
                std::filesystem::path fp{p};
                if (fp.is_absolute()) {
                    return p;
                }
                if (baseDir.empty()) {
                    return p;
                }
                return (baseDir / fp).lexically_normal().string();
            };
            // 一条资源: 唯一的标量字段按 path 处理, 其余字段转成 args JSON
            auto pushEntry = [&](std::string type, const YAML::Node& item) {
                if (type.empty()) {
                    return;
                }
                if (item.IsScalar()) {
                    auto p = resolveRel(item.as<std::string>());
                    if (!p.empty()) {
                        resources->entries.push_back(PluginManifestResources::Entry{
                            .type     = std::move(type),
                            .path     = std::move(p),
                            .argsJson = {}
                        });
                    }
                    return;
                }
                if (!item.IsMap()) {
                    XX_LOGW(
                        "Plugin manifest `{}` resource `{}` entry is neither path nor map, skipped",
                        logHint,
                        type
                    );
                    return;
                }
                PluginManifestResources::Entry entry{.type = std::move(type)};
                utilxx_base::Json              entryArgs = utilxx_base::Json::object();
                for (const auto& kv : item) {
                    auto key = kv.first.as<std::string>();
                    if (key == "type") {
                        continue;
                    }
                    if (key == "path" && kv.second.IsScalar()) {
                        entry.path = resolveRel(kv.second.as<std::string>());
                        continue;
                    }
                    entryArgs[key] = yamlToJson(kv.second);
                }
                if (entryArgs.size() > 0) {
                    entry.argsJson = entryArgs.dump();
                }
                resources->entries.push_back(std::move(entry));
            };
            if (node["resources"] && node["resources"].IsSequence()) {
                for (const auto& item : node["resources"]) {
                    if (!item.IsMap() || !item["type"] || !item["type"].IsScalar()) {
                        XX_LOGW(
                            "Plugin manifest `{}` resource entry missing `type`, skipped",
                            logHint
                        );
                        continue;
                    }
                    pushEntry(item["type"].as<std::string>(), item);
                }
            }
            // 兼容旧写法: 顶层 skill / memory / mcp 段
            for (const char* legacyType : {"skill", "memory", "mcp"}) {
                if (!node[legacyType] || !node[legacyType].IsSequence()) {
                    continue;
                }
                for (const auto& item : node[legacyType]) {
                    pushEntry(legacyType, item);
                }
            }
        }
        // ---- 接口声明段 (require/optional; 见 PluginManifestInterfaces) ----
        // 段缺失/非法项跳过并告警, 不影响 manifest 合法性 (声明是可选增强);
        // 名称仅做非空校验, 语义 (前缀归属/主程序支持集比对) 由加载路径的
        // checkInterfacesForSide 处理 —— 解析与协商解耦, 第三方前缀天然合法
        if (interfaces) {
            *interfaces   = PluginManifestInterfaces{};
            auto readList = [&logHint](
                                const YAML::Node&         section,
                                std::vector<std::string>& out,
                                std::string_view          what
                            ) {
                if (!section || !section.IsSequence()) {
                    return;
                }
                for (const auto& item : section) {
                    if (!item.IsScalar()) {
                        continue;
                    }
                    auto n = item.as<std::string>();
                    if (n.empty()) {
                        XX_LOGW(
                            "Plugin manifest `{}` interfaces.{} has empty name, skipped",
                            logHint,
                            what
                        );
                        continue;
                    }
                    out.push_back(std::move(n));
                }
            };
            if (node["interfaces"] && node["interfaces"].IsMap()) {
                readList(node["interfaces"]["require"], interfaces->require, "require");
                readList(node["interfaces"]["optional"], interfaces->optional, "optional");
            }
        }
    } catch (const std::exception& e) {
        XX_LOGE("Parse plugin manifest `{}` failed: {}", logHint, e.what());
        return false;
    }
    // `entry` 可以缺省: JS 插件缺省用 plugin.js, 动态库插件的入口按平台默认库名与
    // 目录里唯一的库文件解析 (见 host/loader 与主程序的多目标分支选择)。
    if (name.empty()) {
        XX_LOGE("Plugin manifest `{}` invalid: name required", logHint);
        return false;
    }
    return true;
}
} // namespace

bool parsePluginManifest(
    const std::filesystem::path& dir,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends,
    PluginManifestResources*     resources,
    PluginManifestInterfaces*    interfaces
) {
    auto            yamlPath = dir / "plugin.yaml";
    std::error_code ec;
    if (!std::filesystem::exists(yamlPath, ec)) {
        return false;
    }
    try {
        auto node = YAML::LoadFile(yamlPath.string());
        return parseManifestNode(
            node,
            dir,
            yamlPath.string(),
            name,
            entry,
            depends,
            optionalDepends,
            resources,
            interfaces
        );
    } catch (const std::exception& e) {
        XX_LOGE("Parse plugin manifest `{}` failed: {}", yamlPath.string(), e.what());
        return false;
    }
}

bool parsePluginManifestFromString(
    const std::string&           yamlStr,
    const std::filesystem::path& baseDir,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends,
    PluginManifestResources*     resources,
    PluginManifestInterfaces*    interfaces
) {
    if (yamlStr.empty()) {
        return false;
    }
    try {
        auto        node = YAML::Load(yamlStr);
        std::string hint = baseDir.empty() ? std::string{"<builtin manifest>"}
                                           : (baseDir / "plugin.yaml").string();
        return parseManifestNode(
            node,
            baseDir,
            hint,
            name,
            entry,
            depends,
            optionalDepends,
            resources,
            interfaces
        );
    } catch (const std::exception& e) {
        XX_LOGE("Parse builtin manifest failed: {}", e.what());
        return false;
    }
}

bool parseBuiltinManifest(
    std::string_view          pluginName,
    std::string&              name,
    std::string&              entry,
    std::vector<std::string>& depends,
    std::vector<std::string>& optionalDepends,
    PluginManifestResources*  resources,
    PluginManifestInterfaces* interfaces
) {
    auto* m = findBuiltinManifest(pluginName);
    if (!m || pluginStringView2std(m->yaml).empty()) {
        return false;
    }
    // 内嵌清单的资源相对路径无需按插件目录解析 (baseDir 为空)
    return parsePluginManifestFromString(
        std::string{pluginStringView2std(m->yaml)},
        std::filesystem::path{},
        name,
        entry,
        depends,
        optionalDepends,
        resources,
        interfaces
    );
}

std::string resolvePluginEntryPath(const std::filesystem::path& dir, const std::string& entry) {
    // 平台化: manifest 按 Linux 书写 (libfoo.so), Windows/macOS 下修正扩展名
    auto fixExt = [](std::string p) -> std::string {
#if XX_IS_WIN_D
        if (p.ends_with(".so")) {
            p.replace(p.size() - 3, 3, ".dll");
        }
#elif XX_IS_IOS_D || XX_IS_MACOS_D
        if (p.ends_with(".so")) {
            p.replace(p.size() - 3, 3, ".dylib");
        }
#endif
        return p;
    };
    auto            entryPath = fixExt((dir / entry).lexically_normal().string());
    std::error_code ec;
    if (!std::filesystem::exists(entryPath, ec)) {
        // 多配置生成器 (MSVC Debug/Release): 产物位于配置子目录
        for (const char* cfg : {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}) {
            auto candidate = fixExt((dir / cfg / entry).lexically_normal().string());
            if (std::filesystem::exists(candidate, ec)) {
                entryPath = std::move(candidate);
                break;
            }
        }
    }
    return entryPath;
}

} // namespace pluginxx
