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

const char* publisherName(BroadcastPublisher_e publish) noexcept {
    return publish == BroadcastPublisher_e::Anyone ? "anyone" : "owner";
}

const char* scopeName(BroadcastScope_e scope) noexcept {
    return scope == BroadcastScope_e::System ? "system" : "app";
}

const char* limitModeName(BroadcastLimitMode_e mode) noexcept {
    return mode == BroadcastLimitMode_e::Drop ? "drop" : "delay";
}

/// 单调时钟毫秒 (系统时间被调整不影响限频)
int64_t steadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

} // namespace

std::string BroadcastRegistry::validateDecl(const BroadcastDecl& decl) {
    if (decl.id.empty()) {
        return "broadcast id is empty";
    }
    if (decl.cache != BroadcastCache_e::None && decl.cache != BroadcastCache_e::Latest) {
        return "broadcast `" + decl.id + "`: unknown cache value";
    }
    if (decl.publish != BroadcastPublisher_e::Owner
        && decl.publish != BroadcastPublisher_e::Anyone) {
        return "broadcast `" + decl.id + "`: unknown publish value";
    }
    if (decl.scope != BroadcastScope_e::App && decl.scope != BroadcastScope_e::System) {
        return "broadcast `" + decl.id + "`: unknown scope value";
    }
    if (decl.scope == BroadcastScope_e::System
        && decl.publish != BroadcastPublisher_e::Owner) {
        // 调试/自省频道只由宿主发布, 插件写成 anyone 只会在发布时被拒
        return "broadcast `" + decl.id + "`: system scope requires publish: owner";
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
    std::string_view publisher,
    std::string*     error
) {
    // 没有注入调度器 (库被单独使用 / 单元测试): delay 的补发退化为"下一次发布时顺便补"
    {
        bool hasScheduler = false;
        {
            std::lock_guard lock(mutex_);
            hasScheduler = static_cast<bool>(scheduler_);
        }
        if (!hasScheduler) {
            flushPending();
        }
    }
    /// 这一轮要立刻投递的通知 (回调在锁外执行)
    struct Delivery {
        std::function<void(std::string_view)> handler;
        std::string                           payload;
    };
    std::vector<Delivery> deliveries;
    int64_t               nextDelayMs = -1;
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
        if (channel.decl.scope == BroadcastScope_e::System && !publisher.empty()) {
            if (error) {
                *error = "broadcast `" + std::string{id} + "` is a system channel";
            }
            XX_LOGW(
                "BroadcastRegistry: plugin `{}` cannot publish system broadcast `{}`",
                publisher,
                id
            );
            return false;
        }
        if (channel.decl.publish == BroadcastPublisher_e::Owner
            && channel.decl.owner != publisher) {
            if (error) {
                *error = "broadcast `" + std::string{id} + "` is owned by `"
                         + (channel.decl.owner.empty() ? std::string{"app"}
                                                       : channel.decl.owner)
                         + "`";
            }
            XX_LOGW(
                "BroadcastRegistry: plugin `{}` cannot publish broadcast `{}` owned by `{}`",
                publisher,
                id,
                channel.decl.owner.empty() ? "app" : channel.decl.owner
            );
            return false;
        }
        if (channel.decl.cache == BroadcastCache_e::Latest) {
            channel.latest.assign(payload.data(), payload.size());
            channel.hasLatest = true;
        }
        channel.lastPublisher.assign(publisher.data(), publisher.size());

        const int64_t now = steadyNowMs();
        for (BroadcastSub& sub : channel.subs) {
            if (!sub.handler) {
                continue;
            }
            const int64_t interval = sub.intervalMs;
            const bool    due      = interval <= 0 || !sub.hasNotified
                                || now - sub.lastNotifyMs >= interval;
            if (due) {
                deliveries.push_back(Delivery{sub.handler, std::string{payload}});
                sub.hasNotified  = true;
                sub.lastNotifyMs = now;
                sub.hasPending   = false;
                sub.pending.clear();
                continue;
            }
            if (sub.mode != BroadcastLimitMode_e::Delay) {
                continue; // drop: 间隔内的通知直接丢掉
            }
            // delay: 只留最新一条, 到点补发 (due 一定 > 0, 因为上面判过还在间隔内)
            sub.pending.assign(payload.data(), payload.size());
            sub.hasPending = true;
            const int64_t waitMs = sub.lastNotifyMs + interval - now;
            if (nextDelayMs < 0 || waitMs < nextDelayMs) {
                nextDelayMs = waitMs;
            }
        }
        if (nextDelayMs >= 0) {
            channel.flushScheduled = true;
            if (!flushArmed_) {
                flushArmed_ = true;
            } else {
                // 已经排过一轮: 不重复排, 那一轮会处理全部频道
                nextDelayMs = -1;
            }
        }
    }
    for (Delivery& item : deliveries) {
        deliverOne(item.handler, item.payload, id);
    }
    if (nextDelayMs >= 0) {
        scheduleFlush(nextDelayMs);
    }
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
    const BroadcastSubscribeOptions&      options,
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
        added.handler    = handler;
        added.intervalMs = options.intervalMs > 0 ? options.intervalMs : 0;
        added.mode       = options.mode == BroadcastLimitMode_e::Drop
                               ? BroadcastLimitMode_e::Drop
                               : BroadcastLimitMode_e::Delay;
        if (channel.decl.pushOnSubscribe && channel.hasLatest) {
            // 订阅即推的当前值算这个订阅的第一条通知, 也是限频窗口的起点
            added.hasNotified   = true;
            added.lastNotifyMs  = steadyNowMs();
            pushNow             = true;
            current             = channel.latest;
            pushHandler         = handler;
        }
        channel.subs.push_back(std::move(added));
    }
    // 订阅即推: 用当前值回调一次 (锁外, 回调里可以再次发布/退订)
    if (pushNow && pushHandler) {
        deliverOne(pushHandler, current, id);
    }
    size_t subscribers = 0;
    {
        std::lock_guard lock(mutex_);
        auto            it = channels_.find(id);
        if (it != channels_.end()) {
            subscribers = it->second.subs.size();
        }
    }
    // 订阅数变化 (只读通知; 回调在锁外, 可以再订阅 / 发布)
    notifySubscribers(id, subscribers);
    return token;
}

bool BroadcastRegistry::unsubscribe(std::string_view id, uint64_t token) {
    size_t subscribers = 0;
    {
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
        if (before == subs.size()) {
            return false;
        }
        // 没有等补发的订阅了: 这一频道不必再参于补发
        if (std::none_of(subs.begin(), subs.end(), [](const BroadcastSub& sub) {
                return sub.hasPending;
            })) {
            it->second.flushScheduled = false;
        }
        subscribers = subs.size();
    }
    notifySubscribers(id, subscribers);
    return true;
}

bool BroadcastRegistry::removeChannel(std::string_view id) {
    const std::string key{id};
    {
        std::lock_guard lock(mutex_);
        if (channels_.erase(key) == 0) {
            return false;
        }
    }
    notifySubscribers(key, 0);
    return true;
}

size_t BroadcastRegistry::removeSubscriber(std::string_view owner) {
    size_t                                    removed = 0;
    std::vector<std::pair<std::string, size_t>> changed;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, channel] : channels_) {
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
            if (before == subs.size()) {
                continue;
            }
            removed += before - subs.size();
            if (std::none_of(subs.begin(), subs.end(), [](const BroadcastSub& sub) {
                    return sub.hasPending;
                })) {
                channel.flushScheduled = false;
            }
            changed.emplace_back(id, subs.size());
        }
    }
    for (const auto& [id, subscribers] : changed) {
        notifySubscribers(id, subscribers);
    }
    return removed;
}

size_t BroadcastRegistry::removeChannelsOwnedBy(std::string_view owner) {
    size_t                   removed = 0;
    std::vector<std::string> removedIds;
    {
        std::lock_guard lock(mutex_);
        for (auto it = channels_.begin(); it != channels_.end();) {
            if (!it->second.decl.isAppChannel() && it->second.decl.owner == owner) {
                removedIds.push_back(it->first);
                it = channels_.erase(it);
                ++removed;
                continue;
            }
            ++it;
        }
    }
    for (const std::string& id : removedIds) {
        notifySubscribers(id, 0);
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
    j["publish"]        = publisherName(channel.decl.publish);
    j["pushOnSubscribe"] = channel.decl.pushOnSubscribe;
    j["coalesce"]       = channel.decl.coalesce;
    j["scope"]          = scopeName(channel.decl.scope);
    j["owner"] = channel.decl.isAppChannel() ? std::string{"app"} : channel.decl.owner;
    j["subscribers"] = static_cast<int64_t>(channel.subs.size());
    j["hasValue"]    = channel.hasLatest;
    if (!channel.lastPublisher.empty()) {
        j["lastPublisher"] = channel.lastPublisher;
    }
    auto subs = utilxx_base::Json::array();
    for (const BroadcastSub& sub : channel.subs) {
        auto item          = utilxx_base::Json::object();
        item["owner"]      = sub.owner;
        item["intervalMs"] = sub.intervalMs;
        item["mode"]       = limitModeName(sub.mode);
        item["pending"]    = sub.hasPending;
        subs.push_back(item);
    }
    j["subs"] = subs;
    return j.dump();
}

std::string BroadcastRegistry::listJson(std::string_view prefix) const {
    std::lock_guard lock(mutex_);
    auto            out   = utilxx_base::Json::object();
    auto            items = utilxx_base::Json::array();
    for (const auto& [id, channel] : channels_) {
        if (!prefix.empty() && id.rfind(prefix, 0) != 0) {
            continue;
        }
        auto item            = utilxx_base::Json::object();
        item["id"]           = id;
        item["cache"]        = cacheName(channel.decl.cache);
        item["publish"]      = publisherName(channel.decl.publish);
        item["scope"]        = scopeName(channel.decl.scope);
        item["owner"]        = channel.decl.isAppChannel() ? std::string{"app"} : channel.decl.owner;
        item["subscribers"]  = static_cast<int64_t>(channel.subs.size());
        item["hasValue"]     = channel.hasLatest;
        if (channel.decl.cache == BroadcastCache_e::Latest && channel.hasLatest) {
            // 当前值原样嵌进 JSON; 不是合法 JSON 的载荷按文本给出
            try {
                item["value"] = utilxx_base::Json::parse(channel.latest);
            } catch (...) {
                item["value"] = channel.latest;
            }
        }
        items.push_back(item);
    }
    out["broadcasts"] = items;
    return out.dump();
}

void BroadcastRegistry::setScheduler(BroadcastScheduler scheduler) {
    std::lock_guard lock(mutex_);
    scheduler_ = std::move(scheduler);
}

void BroadcastRegistry::setSubscriberChangeCallback(SubscriberChangeCallback callback) {
    std::lock_guard lock(mutex_);
    subscriberCallback_ = std::move(callback);
}

void BroadcastRegistry::notifySubscribers(std::string_view id, size_t subscribers) const {
    SubscriberChangeCallback callback;
    {
        std::lock_guard lock(mutex_);
        callback = subscriberCallback_;
    }
    if (!callback) {
        return;
    }
    try {
        callback(id, subscribers);
    } catch (const std::exception& e) {
        XX_LOGW("Broadcast `{}` subscriber callback threw: {}", id, e.what());
    } catch (...) {
        XX_LOGW("Broadcast `{}` subscriber callback threw: unknown exception", id);
    }
}

void BroadcastRegistry::flushPending() {
    struct Delivery {
        std::function<void(std::string_view)> handler;
        std::string                           payload;
        std::string                           id;
    };
    std::vector<Delivery> deliveries;
    int64_t               nextDelayMs = -1;
    {
        std::lock_guard lock(mutex_);
        flushArmed_       = false;
        const int64_t now = steadyNowMs();
        for (auto& [id, channel] : channels_) {
            if (!channel.flushScheduled) {
                continue;
            }
            channel.flushScheduled = false;
            bool    anyPending     = false;
            int64_t minWait        = -1;
            for (BroadcastSub& sub : channel.subs) {
                if (!sub.hasPending) {
                    continue;
                }
                const int64_t due = sub.lastNotifyMs + sub.intervalMs;
                if (sub.intervalMs <= 0 || now >= due) {
                    if (sub.handler) {
                        deliveries.push_back(Delivery{sub.handler, sub.hasPending ? sub.pending : std::string{}, id});
                    }
                    sub.hasPending   = false;
                    sub.pending.clear();
                    sub.hasNotified  = true;
                    sub.lastNotifyMs = now;
                    continue;
                }
                anyPending          = true;
                const int64_t waitMs = due - now;
                if (minWait < 0 || waitMs < minWait) {
                    minWait = waitMs;
                }
            }
            if (anyPending) {
                channel.flushScheduled = true;
                if (minWait >= 0 && (nextDelayMs < 0 || minWait < nextDelayMs)) {
                    nextDelayMs = minWait;
                }
            }
        }
        if (nextDelayMs >= 0) {
            flushArmed_ = true;
        }
    }
    for (Delivery& item : deliveries) {
        deliverOne(item.handler, item.payload, item.id);
    }
    if (nextDelayMs >= 0) {
        scheduleFlush(nextDelayMs);
    }
}

void BroadcastRegistry::reset() {
    std::lock_guard lock(mutex_);
    channels_.clear();
    flushArmed_ = false;
}

void BroadcastRegistry::scheduleFlush(int64_t delayMs) {
    BroadcastScheduler   scheduler;
    {
        std::lock_guard lock(mutex_);
        scheduler = scheduler_;
    }
    if (!scheduler) {
        // 没有调度器: 补发退化为"下一次发布时顺便补" (由 publish 的 flushArmed_ 兜住)
        std::lock_guard lock(mutex_);
        flushArmed_ = false;
        return;
    }
    scheduler(delayMs > 0 ? delayMs : 0, [this]() {
        flushPending();
    });
}

void BroadcastRegistry::deliverOne(
    const std::function<void(std::string_view)>& handler,
    std::string_view                             payload,
    std::string_view                             id
) {
    if (!handler) {
        return;
    }
    try {
        handler(payload);
    } catch (const std::exception& e) {
        XX_LOGW("Broadcast `{}` subscriber threw: {}", id, e.what());
    } catch (...) {
        XX_LOGW("Broadcast `{}` subscriber threw: unknown exception", id);
    }
}

} // namespace pluginxx
