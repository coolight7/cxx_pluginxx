/// 插件形态（kind）注册表（主程序侧，与具体应用无关）
///
/// 背景：框架自己只实现**动态库插件**，解释执行的形态（脚本运行器）由框架自带一份或由插件
/// 登记。运行器不该写死在代码里 —— 把脚本运行时做成动态库插件、以后接别的解释器，都不应该改
/// 扫描 / 加载 / 判定的分支。因此插件清单里的 `kind` 一律先查这张表：
/// - 每一项声明：谁提供（框架自己 / 插件实例名）、这个形态自己的**接口代次对**
///   （当前代次 + 兼容下限，清单的 `api_version` 按它判）、交给提供者加载 / 卸载 / 校验的
///   能力全名、一行说明；
/// - 框架自带的形态由主程序在构造时传入（`native` 与它自带的脚本运行器）；
/// - 插件通过接口表 `<应用名>.kind`（`pluginxx/api/kinds.h`）登记自己的形态；
///   **框架自带的解释执行形态可以被插件接管**（`overrideBuiltin`），撤销后自动回到自带实现；
/// - 跟着插件生命周期撤销（禁用 / 卸载 / 关闭插件框架），不写磁盘。
///
/// 判定"由谁实现"只看 `provider`：空 = 框架自己，非空 = 由这个插件托管。
/// "这个包能不能作为这种形态跑"由提供者回答（`validateCapability`，可选）：框架不按形态名
/// 写分支，只负责把目录交给提供者，拿到拒绝理由就原样告诉用户。
///
/// 线程约定：与其它插件注册同一串行上下文（框架 IO 线程）读写，不加锁。
#ifndef PLUGINXX_HOST_KIND_REGISTRY_H
#define PLUGINXX_HOST_KIND_REGISTRY_H

#include "pluginxx/api/app.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pluginxx {

/// 一种插件形态的声明
struct PluginxxKindSpec {
    /// 形态名（清单里的 `kind`；小写字母、数字、`_` `-` `.`）
    std::string kind;

    /// 是否是框架自带的形态名。接管自带形态时仍为 true，但 `provider` 会写上接管它的插件
    bool builtin = false;

    /// 该形态的插件是否按 C ABI 编译
    /// - true：插件是动态库，会调通用接口表（接口代次对直接用框架的 C 接口那一对）；
    /// - false：插件由某个运行器解释执行（脚本 / 解释器 …），不碰 C 接口，用自己声明的代次对。
    ///
    /// 它同时决定要不要按动态库的样子解析多目标分支（见
    /// `pluginxx/host/manifest_targets.h`）。
    bool requiresPluginAbi = true;

    /// 该形态的**接口代次**（框架 / 运行器上报给插件；`0` = 未声明）
    int32_t interfaceVersion = 0;

    /// 该形态能接受的**最低**接口代次（清单 `api_version` 低于它 = 这个插件跑不了；
    /// `0` = 不判）。只有破坏性变更才抬它。
    int32_t minInterfaceVersion = 0;

    /// 提供者（插件实例名；空 = 框架自己实现）
    std::string provider;

    /// 交给提供者加载 / 卸载一个该形态插件的能力全名
    /// （必须在提供者自己的命名空间 `plugin.<插件id>.` 下；空 = 未声明，加载会被拒绝）
    std::string loadCapability;
    std::string unloadCapability;

    /// 交给提供者的**校验**能力全名（可选；同样要在提供者自己的命名空间下）
    ///
    /// 加载一个该形态的插件之前先问它一次：回答 `{"ok":true}` = 可以跑，
    /// `{"ok":false,"error":"..."}` = 拒绝（理由直接给用户）。空 = 不做这一步。
    std::string validateCapability;

    /// 一行说明（自省与管理页展示）
    std::string describe;

    /// 是否由框架自己实现（`provider` 为空）
    bool frameworkOwned() const { return provider.empty(); }
};

/// 形态注册表（一个插件框架实例一份）
///
/// - `builtins`：框架自带的形态声明（顺序即列表顺序），一般由主程序用自己那对 C 接口代次
///   与脚本运行器代次拼出来；
/// - `defaultInterfaceVersion`：未登记的形态、以及"按 C ABI 编译的形态"使用的代次对
///   （主程序传自己的 C 接口代次）。
class PluginxxKindRegistry {
public:
    explicit PluginxxKindRegistry(std::vector<PluginxxKindSpec> builtins = {},
                                  int32_t defaultInterfaceVersion = 0)
        : builtins_(std::move(builtins)),
          defaultInterfaceVersion_(defaultInterfaceVersion) {}

    /// 登记一种形态（插件侧）
    ///
    /// - `overrideBuiltin = true` 时允许接管框架自带的形态（例如把脚本运行器搬进插件）：
    ///   `requiresPluginAbi` 与接口代次对沿用自带声明（解释执行的形态不让接管者改代次），
    ///   撤销后回到自带实现；
    /// - 解释执行的形态自己声明 `interfaceVersion` / `minInterfaceVersion`（不声明 = 不判）；
    ///   会调 C 接口表的形态（`requiresPluginAbi`）一律用框架的 C 接口那一对；
    /// - 同一个提供者重复登记 = 幂等覆盖；别的插件已占用同名形态则拒绝。
    bool registerKind(const PluginxxKindSpec& spec, bool overrideBuiltin, std::string& err) {
        if (!isValidKindName(spec.kind)) {
            err = "invalid plugin kind name (lowercase letters, digits, `_`, `-`, `.`; 1..32 chars)";
            return false;
        }
        if (spec.provider.empty()) {
            err = "plugin kind `" + spec.kind + "` has no provider";
            return false;
        }
        if (spec.loadCapability.empty()) {
            err = "plugin kind `" + spec.kind + "` has no loadCapability";
            return false;
        }
        PluginxxKindSpec entry = spec;
        if (const PluginxxKindSpec* builtin = findBuiltin(spec.kind)) {
            if (!overrideBuiltin) {
                err = "plugin kind `" + spec.kind +
                      "` is built into the framework; set overrideBuiltin to true to take it over";
                return false;
            }
            entry.builtin = true;
            // 形态本身是否按 C ABI 编译、接口代次对都由框架决定，不让接管者改
            entry.requiresPluginAbi  = builtin->requiresPluginAbi;
            entry.interfaceVersion   = builtin->interfaceVersion;
            entry.minInterfaceVersion = builtin->minInterfaceVersion;
            // 只能接管"解释执行"的自带形态：动态库的加载与生命周期是框架自己的职责，
            // 换出去就没法保证注入 / 卸载纪律
            if (builtin->requiresPluginAbi) {
                err = "plugin kind `" + spec.kind +
                      "` is a native-library kind and cannot be taken over by a plugin";
                return false;
            }
        } else if (entry.requiresPluginAbi) {
            // 会调 C 接口表的形态（运行器自己编插件）：接口代次对就是 C 接口那一对，
            // 不让登记方随便写（写了也不采信）
            entry.interfaceVersion    = defaultInterfaceVersion_;
            entry.minInterfaceVersion = defaultInterfaceVersion_;
        }
        // 解释执行的形态自己声明代次对：下限不能高于当前代次（未声明 = 不判）
        if (entry.interfaceVersion < 0 || entry.minInterfaceVersion < 0) {
            err = "plugin kind `" + spec.kind + "` interface versions must not be negative";
            return false;
        }
        if (entry.interfaceVersion > 0 && entry.minInterfaceVersion > entry.interfaceVersion) {
            err = "plugin kind `" + spec.kind +
                  "` minInterfaceVersion must not be greater than interfaceVersion";
            return false;
        }
        if (const PluginxxKindSpec* existing = findOverride(spec.kind)) {
            if (existing->provider != spec.provider) {
                err = "plugin kind `" + spec.kind + "` is already provided by plugin `" +
                      existing->provider + "`";
                return false;
            }
        }
        for (auto& item : overrides_) {
            if (item.kind == spec.kind) {
                item = std::move(entry);
                return true;
            }
        }
        overrides_.push_back(std::move(entry));
        return true;
    }

    /// 撤销一种形态（只能撤销自己登记的；接管自带形态的撤销后自动回到自带实现）
    bool unregisterKind(std::string_view kind, std::string_view provider, std::string& err) {
        const auto it = std::find_if(overrides_.begin(), overrides_.end(),
                                     [&](const PluginxxKindSpec& item) {
                                         return item.kind == kind;
                                     });
        if (it == overrides_.end() || it->provider != provider) {
            err = "plugin kind `" + std::string{kind} + "` was not registered by this plugin";
            return false;
        }
        overrides_.erase(it);
        return true;
    }

    /// 插件停用 / 卸载：撤销它登记的全部形态
    void unregisterAllOf(std::string_view provider) {
        overrides_.erase(
            std::remove_if(overrides_.begin(), overrides_.end(),
                           [&](const PluginxxKindSpec& item) { return item.provider == provider; }),
            overrides_.end());
    }

    /// 查一种形态（未登记返回 nullptr）；插件接管了自带形态时给出接管后的那一项
    const PluginxxKindSpec* find(std::string_view kind) const {
        if (const PluginxxKindSpec* override = findOverride(kind)) {
            return override;
        }
        return findBuiltin(kind);
    }

    /// 全部形态（框架自带的在前；被接管的显示接管后的那一项）
    std::vector<PluginxxKindSpec> all() const {
        std::vector<PluginxxKindSpec> result;
        result.reserve(builtins_.size() + overrides_.size());
        for (const PluginxxKindSpec& builtin : builtins_) {
            const PluginxxKindSpec* override = findOverride(builtin.kind);
            result.push_back(override ? *override : builtin);
        }
        for (const PluginxxKindSpec& item : overrides_) {
            if (!item.builtin) {
                result.push_back(item);
            }
        }
        return result;
    }

    /// 该形态的插件是否按 C ABI 编译
    /// - **未登记的形态按 true 算**：宁可要求插件声明够新的接口版本，也不要放进来一个
    ///   框架不知道是怎么编译的二进制。
    bool requiresPluginAbi(std::string_view kind) const {
        const PluginxxKindSpec* spec = find(kind);
        return spec ? spec->requiresPluginAbi : true;
    }

    /// 该形态能接受的最低接口代次（清单 `api_version` 低于它 = 跑不了）
    ///
    /// - 已登记但没声明（`0`）= 不判（返回 0）；
    /// - **未登记的形态按 C 接口下限算**，与 [requiresPluginAbi] 同一个理由。
    int32_t minInterfaceVersion(std::string_view kind) const {
        const PluginxxKindSpec* spec = find(kind);
        return spec ? spec->minInterfaceVersion : defaultInterfaceVersion_;
    }

    /// 该形态当前上报的接口代次（插件读到的 `apiVersion`）
    ///
    /// 未登记或未声明的形态回退到 C 接口代次。
    int32_t interfaceVersion(std::string_view kind) const {
        const PluginxxKindSpec* spec = find(kind);
        return (spec && spec->interfaceVersion > 0) ? spec->interfaceVersion
                                                    : defaultInterfaceVersion_;
    }

    /// 形态名是否合法（清单字段与接口表共用这一份规则）
    static bool isValidKindName(std::string_view kind) {
        if (kind.empty() || kind.size() > 32) {
            return false;
        }
        for (const char c : kind) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                            c == '-' || c == '.';
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    /// 框架自带的形态声明（顺序即列表顺序）
    const std::vector<PluginxxKindSpec>& builtins() const { return builtins_; }

    /// 框架的默认接口代次（未登记形态与 C ABI 形态使用）
    int32_t defaultInterfaceVersion() const { return defaultInterfaceVersion_; }

private:
    const PluginxxKindSpec* findBuiltin(std::string_view kind) const {
        const auto it = std::find_if(builtins_.begin(), builtins_.end(),
                                     [&](const PluginxxKindSpec& item) {
                                         return item.kind == kind;
                                     });
        return it == builtins_.end() ? nullptr : &*it;
    }

    const PluginxxKindSpec* findOverride(std::string_view kind) const {
        const auto it = std::find_if(overrides_.begin(), overrides_.end(),
                                     [&](const PluginxxKindSpec& item) {
                                         return item.kind == kind;
                                     });
        return it == overrides_.end() ? nullptr : &*it;
    }

    /// 框架自带的形态
    std::vector<PluginxxKindSpec> builtins_;
    /// 插件登记的形态（含接管自带形态的那一项）
    std::vector<PluginxxKindSpec> overrides_;
    /// 未登记形态 / C ABI 形态使用的接口代次
    int32_t defaultInterfaceVersion_ = 0;
};

} // namespace pluginxx

#endif /* PLUGINXX_HOST_KIND_REGISTRY_H */
