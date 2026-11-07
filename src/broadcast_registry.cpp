#include "pluginxx/host/broadcast_registry.h"

#include "utilxx_base/container_util.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace pluginxx {

namespace {

/// 把声明里的档位写成 JSON 用的文本
const char* cacheName(BroadcastCache_e cache) noexcept {
    return cache == BroadcastCache_e::Latest ? "latest" : "none";
}

const char* writableName(BroadcastWritable_e writable) noexcept {
    return writable == BroadcastWritable_e::Anyone ? "anyone" : "owner";
}

const char* scopeName(BroadcastScope_e scope) noexcept {
    return scope == BroadcastScope_e::System ? "system" : "app";
}

} // namespace

std::string BroadcastRegistry::validateDecl(const BroadcastDecl& decl) {
    if (decl.id.empty()) {
        return "broadcast id is empty";
    }
    if (decl.cache != BroadcastCache_e::None && decl.cache != BroadcastCache_e::Latest) {
        return "broadcast `" + decl.id + "`: unknown cache value";
    }
    if (decl.writable != BroadcastWritable_e::Owner && decl.writable != BroadcastWritable_e::Anyone) {
        return "broadcast `" + decl.id + "`: unknown writable value";
    }
    if (decl.scope != BroadcastScope_e::App && decl.scope != BroadcastScope_e::System) {
        return "broadcast `" + decl.id + "`: unknown scope value";
    }
    if (decl.scope == BroadcastScope_e::System && decl.writable != BroadcastWritable_e::Owner) {
        // 调试/自省频道只由宿主写, 插件写成 anyone 只会在发布时被拒
        return "broadcast `" + decl.id + "`: system scope requires writable: owner";
    }
    if (decl.pushOnSubscribe && decl.cache != BroadcastCache_e::Latest) {
        // 订阅时没有当前值可推, 声明自相矛盾 (生成物也会拦, 这里兜住运行期声明)
        return "broadcast `" + decl.id + "`: pushOnSubscribe requires cache: latest";
    }
    return {};
}

bool BroadcastRegistry::declare(const BroadcastDecl& inDecl, std::string* error) {
    const std::string invalid = validateDecl(inDecl);
    if (!invalid.empty()) {
        if (error) {
            *error = invalid;
        }
        return false;
    }

    std::lock_guard lock(mutex_);
    auto            it = channels_.find(inDecl.id);
    if (it == channels_.end()) {
        BroadcastChannel channel;
        channel.decl = inDecl;
        utilxx_base::insertHeterogeneous(channels_, inDecl.id, std::move(channel));
        return true;
    }

    BroadcastChannel& channel = it->second;
    if (channel.decl.isAppChannel() && !inDecl.isAppChannel()) {
        if (error) {
            *error = "broadcast `" + inDecl.id + "` is declared by the application";
        }
        XX_LOGW(
            "BroadcastRegistry: broadcast `{}` is declared by the application, "
            "plugin declaration ignored",
            inDecl.id
        );
        return false;
    }
    XX_LOGI(
        "BroadcastRegistry: declaration of `{}` replaced ({} -> {})",
        inDecl.id,
        channel.decl.owner.empty() ? "app" : channel.decl.owner,
        inDecl.owner.empty() ? "app" : inDecl.owner
    );
    channel.decl = inDecl;
    if (inDecl.cache != BroadcastCache_e::Latest) {
        channel.latest.clear();
        channel.hasLatest = false;
    }
    return true;
}

bool BroadcastRegistry::declares(std::string_view id) const {
    std::lock_guard lock(mutex_);
    return channels_.contains(id);
}

const BroadcastDecl* BroadcastRegistry::declOf(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto            it = channels_.find(id);
    return it == channels_.end() ? nullptr : &it->second.decl;
}

bool BroadcastRegistry::publish(
    std::string_view id,
    std::string_view payload,
    std::string_view writer,
    std::string*     error
) {
    std::vector<BroadcastSub> subs;
    {
        std::lock_guard lock(mutex_);
        auto            it = channels_.find(id);
        if (it == channels_.end()) {
            if (error) {
                *error = "broadcast `" + std::string{id} + "` is not declared";
            }
            return false;
        }
        BroadcastChannel& channel = it->second;
        if (channel.decl.scope == BroadcastScope_e::System && !writer.empty()) {
            if (error) {
                *error = "broadcast `" + std::string{id} + "` is a system channel";
            }
            XX_LOGW(
                "BroadcastRegistry: plugin `{}` cannot write system broadcast `{}`",
                writer,
                id
            );
            return false;
        }
        if (channel.decl.writable == BroadcastWritable_e::Owner
            && channel.decl.owner != writer) {
            if (error) {
                *error = "broadcast `" + std::string{id} + "` is owned by `"
                         + (channel.decl.owner.empty() ? std::string{"app"} : channel.decl.owner)
                         + "`";
            }
            XX_LOGW(
                "BroadcastRegistry: plugin `{}` cannot write broadcast `{}` owned by `{}`",
                writer,
                id,
                channel.decl.owner.empty() ? "app" : channel.decl.owner
            );
            return false;
        }
        if (channel.decl.cache == BroadcastCache_e::Latest) {
            channel.latest.assign(payload.data(), payload.size());
            channel.hasLatest = true;
        }
        channel.lastWriter.assign(writer.data(), writer.size());
        subs = channel.subs;
    }
    deliver(subs, payload, id);
    return true;
}

bool BroadcastRegistry::read(std::string_view id, std::string* out) const {
    std::lock_guard lock(mutex_);
    auto            it = channels_.find(id);
    if (it == channels_.end() || it->second.decl.cache != BroadcastCache_e::Latest
        || !it->second.hasLatest) {
        return false;
    }
    if (out) {
        *out = it->second.latest;
    }
    return true;
}

uint64_t BroadcastRegistry::subscribe(
    std::string_view                      id,
    std::string_view                      owner,
    std::function<void(std::string_view)> handler,
    std::string*                          error
) {
    if (!handler) {
        if (error) {
            *error = "broadcast subscribe: handler is null";
        }
        return 0;
    }
    uint64_t                              token   = 0;
    bool                                  pushNow = false;
    std::string                           current;
    std::function<void(std::string_view)> pushHandler;
    {
        std::lock_guard lock(mutex_);
        auto            it = channels_.find(id);
        if (it == channels_.end()) {
            if (error) {
                *error = "broadcast `" + std::string{id} + "` is not declared";
            }
            return 0;
        }
        BroadcastChannel& channel = it->second;
        token                     = ++channel.nextToken;
        BroadcastSub added;
        added.token = token;
        added.owner.assign(owner.data(), owner.size());
        added.handler = handler;
        channel.subs.push_back(std::move(added));
        if (channel.decl.pushOnSubscribe && channel.hasLatest) {
            pushNow     = true;
            current     = channel.latest;
            pushHandler = handler;
        }
    }
    // 订阅即推: 用当前值回调一次 (锁外, 回调里可以再次发布/退订)
    if (pushNow && pushHandler) {
        try {
            pushHandler(current);
        } catch (const std::exception& e) {
            XX_LOGW("Broadcast `{}` subscriber threw on subscribe: {}", id, e.what());
        } catch (...) {
            XX_LOGW("Broadcast `{}` subscriber threw on subscribe: unknown exception", id);
        }
    }
    return token;
}

bool BroadcastRegistry::unsubscribe(std::string_view id, uint64_t token) {
    std::lock_guard lock(mutex_);
    auto            it = channels_.find(id);
    if (it == channels_.end()) {
        return false;
    }
    auto&      subs   = it->second.subs;
    const auto before = subs.size();
    subs.erase(
        std::remove_if(
            subs.begin(),
            subs.end(),
            [token](const BroadcastSub& sub) {
                return sub.token == token;
            }
        ),
        subs.end()
    );
    return before != subs.size();
}

size_t BroadcastRegistry::removeSubscriber(std::string_view owner) {
    std::lock_guard lock(mutex_);
    size_t          removed = 0;
    for (auto& [id, channel] : channels_) {
        (void)id;
        auto&      subs   = channel.subs;
        const auto before = subs.size();
        subs.erase(
            std::remove_if(
                subs.begin(),
                subs.end(),
                [owner](const BroadcastSub& sub) {
                    return sub.owner == owner;
                }
            ),
            subs.end()
        );
        removed += before - subs.size();
    }
    return removed;
}

size_t BroadcastRegistry::removeChannelsOwnedBy(std::string_view owner) {
    std::lock_guard lock(mutex_);
    size_t          removed = 0;
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (!it->second.decl.isAppChannel() && it->second.decl.owner == owner) {
            it = channels_.erase(it);
            ++removed;
            continue;
        }
        ++it;
    }
    return removed;
}

std::vector<std::string> BroadcastRegistry::ids() const {
    std::lock_guard          lock(mutex_);
    std::vector<std::string> out;
    out.reserve(channels_.size());
    for (const auto& [id, channel] : channels_) {
        (void)channel;
        out.push_back(id);
    }
    return out;
}

std::string BroadcastRegistry::infoJson(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto            it = channels_.find(id);
    if (it == channels_.end()) {
        return {};
    }
    const auto& channel = it->second;
    auto        j       = utilxx_base::Json::object();
    j["id"]             = channel.decl.id;
    j["cache"]          = cacheName(channel.decl.cache);
    j["writable"]       = writableName(channel.decl.writable);
    j["pushOnSubscribe"] = channel.decl.pushOnSubscribe;
    j["coalesce"]       = channel.decl.coalesce;
    j["scope"]          = scopeName(channel.decl.scope);
    j["owner"] = channel.decl.isAppChannel() ? std::string{"app"} : channel.decl.owner;
    j["subscribers"] = static_cast<int64_t>(channel.subs.size());
    j["hasValue"]    = channel.hasLatest;
    if (!channel.lastWriter.empty()) {
        j["lastWriter"] = channel.lastWriter;
    }
    return j.dump();
}

std::string BroadcastRegistry::listJson() const {
    std::lock_guard lock(mutex_);
    auto            out   = utilxx_base::Json::object();
    auto            items = utilxx_base::Json::array();
    for (const auto& [id, channel] : channels_) {
        auto item            = utilxx_base::Json::object();
        item["id"]           = id;
        item["cache"]        = cacheName(channel.decl.cache);
        item["writable"]     = writableName(channel.decl.writable);
        item["scope"]        = scopeName(channel.decl.scope);
        item["subscribers"]  = static_cast<int64_t>(channel.subs.size());
        item["hasValue"]     = channel.hasLatest;
        items.push_back(item);
    }
    out["broadcasts"] = items;
    return out.dump();
}

void BroadcastRegistry::reset() {
    std::lock_guard lock(mutex_);
    channels_.clear();
}

void BroadcastRegistry::deliver(
    const std::vector<BroadcastSub>& subs,
    std::string_view                 payload,
    std::string_view                 id
) {
    for (const auto& sub : subs) {
        if (!sub.handler) {
            continue;
        }
        try {
            sub.handler(payload);
        } catch (const std::exception& e) {
            XX_LOGW("Broadcast `{}` subscriber threw: {}", id, e.what());
        } catch (...) {
            XX_LOGW("Broadcast `{}` subscriber threw: unknown exception", id);
        }
    }
}

} // namespace pluginxx
