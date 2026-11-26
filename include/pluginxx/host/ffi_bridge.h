/// 主程序 ↔ 插件框架的边界件（与具体应用无关）
///
/// 只放"任何主程序都要写一遍"的三件事，**不含 op 分发**（那是各主程序自己的兜底通道）：
///
/// - [EventQueue]：事件入队与取走。"任何线程 push、单消费者 poll"，队列有界；
///   入队后调用唤醒回调（**在锁外**调用，回调里可以再加锁，不会死锁）；
/// - [RequestRegistry]：一次请求的记账。"受理 ⇒ 恰好一条结果"，`settle` 只有一个赢家；
///   取消幂等；停机 / 卸载时把还没收尾的请求整体结算。**库内不放定时器**：
///   `nextDeadlineMs()` / `popExpired()` 交给调用方用自己的 io_context 或定时器驱动；
/// - 跨堆字符串：`strdupTo` / `freeString`，分配与释放都用调用方给的分配器
///   （主程序与库可能各有一套 CRT，库不替调用方决定用哪个堆）。
///
/// 线程约定：两个类都可以在任意线程调用，内部加锁；`poll` 属于单消费者（主程序的事件泵）。
#ifndef PLUGINXX_HOST_FFI_BRIDGE_H
#define PLUGINXX_HOST_FFI_BRIDGE_H

#include "pluginxx/api/abi.h"
#include "pluginxx/api/result.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pluginxx {
namespace ffi {

/// ==================== 事件队列 ====================
///
/// 事件是**已经序列化好的 JSON 文本**（形状由主程序决定，桥不解释），`poll` 把它们拼成一个
/// JSON 数组交给主程序。`seq` / 时间戳 / 类型字段也由主程序在序列化时写进去，桥只负责排队不乱序。
class EventQueue {
public:
    explicit EventQueue(size_t capacity = 16384) : capacity_(capacity == 0 ? 1 : capacity) {}

    /// 改容量（启动时按配置设置一次；已经排队的事件不受影响）
    void setCapacity(size_t capacity) {
        std::lock_guard<std::mutex> lock{mutex_};
        capacity_ = capacity == 0 ? 1 : capacity;
    }

    size_t capacity() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return capacity_;
    }

    /// 注册唤醒回调（入队时触发；**在锁外调用**，必须快速返回）
    void setWake(std::function<void()> wake) {
        std::lock_guard<std::mutex> lock{mutex_};
        wake_ = std::move(wake);
    }

    /// 入队一条事件；队列满返回 false 且不丢任何事件（调用方回 `busy` 或自行降级）
    bool push(std::string jsonEvent) {
        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (events_.size() >= capacity_) {
                return false;
            }
            events_.push_back(std::move(jsonEvent));
            wake = wake_;
        }
        if (wake) {
            wake();
        }
        return true;
    }

    /// 入队一条事件；队列满时丢最旧的一条再入队（返回 false 表示"丢过东西"）
    ///
    /// 调用方通常会在返回 false 时再补一条"队列溢出"事件，让主程序知道丢了什么。
    /// `droppedCount` 给出累计丢弃条数。
    bool pushDropOldest(std::string jsonEvent, size_t* droppedCount = nullptr) {
        std::function<void()> wake;
        bool                  droppedOne = false;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (events_.size() >= capacity_) {
                events_.pop_front();
                ++dropped_;
                droppedOne = true;
            }
            events_.push_back(std::move(jsonEvent));
            if (droppedCount) {
                *droppedCount = dropped_;
            }
            wake = wake_;
        }
        if (wake) {
            wake();
        }
        return !droppedOne;
    }

    /// 取最多 `maxCount` 条事件，拼成 JSON 数组；返回取到的条数（`0` = 没有事件，`outJsonArray` 不变）
    size_t poll(size_t maxCount, std::string& outJsonArray) {
        std::lock_guard<std::mutex> lock{mutex_};
        if (events_.empty()) {
            return 0;
        }
        const size_t count =
            maxCount == 0 ? events_.size() : (std::min)(maxCount, events_.size());
        std::string joined;
        joined.reserve(count * 128);
        joined.push_back('[');
        for (size_t i = 0; i < count; ++i) {
            if (i != 0) {
                joined.push_back(',');
            }
            // 事件本身就是合法 JSON 文本，直接拼；非法文本由生产方负责
            joined += events_.front();
            events_.pop_front();
        }
        joined.push_back(']');
        outJsonArray = std::move(joined);
        return count;
    }

    /// 丢最旧的 `count` 条事件（队列满时生产方要腾位置；计入 `dropped()`）
    void dropOldest(size_t count) {
        std::lock_guard<std::mutex> lock{mutex_};
        const size_t                n = (std::min)(count, events_.size());
        for (size_t i = 0; i < n; ++i) {
            events_.pop_front();
            ++dropped_;
        }
    }

    /// 队列满时：丢最旧的一条，再入队“溢出提示”与本体两条（溢出提示在前）
    ///
    /// 队列允许瞬时到 `capacity + 1` 条（与“丢一条、补一条溢出提示、再入队本体”的旧实现
    /// 一致）。返回 true = 没有丢任何事件；`droppedCount` 给出累计丢弃条数。
    bool pushWithOverflow(std::string event, std::string overflowEvent,
                          size_t* droppedCount = nullptr) {
        std::function<void()> wake;
        bool                  droppedAny = false;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            while (events_.size() + 1 > capacity_) {
                events_.pop_front();
                ++dropped_;
                droppedAny = true;
            }
            events_.push_back(std::move(overflowEvent));
            events_.push_back(std::move(event));
            if (droppedCount) {
                *droppedCount = dropped_;
            }
            wake = wake_;
        }
        if (wake) {
            wake();
        }
        return !droppedAny;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return events_.empty();
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return events_.size();
    }
    size_t dropped() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return dropped_;
    }

private:
    mutable std::mutex      mutex_;
    std::deque<std::string> events_;
    size_t                  capacity_ = 16384;
    size_t                  dropped_  = 0;
    std::function<void()>   wake_;
};

/// ==================== 一次请求的记账 ====================
///
/// 一条记账 = 一次已受理、还没收尾的请求。主程序拿到 id 后无论走哪条路径都要收尾一次：
/// 正常完成、失败、取消、插件卸载、框架停机。`settle` 只有一个赢家（由锁保证），
/// 所以"恰好一条结果"不依赖调用方自觉。
///
/// `deadlineMs` 只是记账：**库不建定时器**。主程序用自己的事件循环等到
/// `nextDeadlineMs()`，再调 `popExpired()` 结算到期的请求（或者干脆不用超时）。
struct RequestEntry {
    int64_t     id        = 0;
    std::string op;        ///< 操作名（主程序自己解释）
    std::string owner;     ///< 所属插件 / 会话 id（空 = 不限）。卸载时按它批量结算
    int64_t     startedMs = 0; ///< 受理时刻（`nowMs` 由调用方给；只用于统计与超时）
    int64_t     deadlineMs = 0; ///< 到期时刻（0 = 不限）
};

class RequestRegistry {
public:
    /// 受理一次请求；返回请求号（从 1 开始单调递增，不复用）
    ///
    /// - `timeoutMs > 0` 时记一条到期时刻（`nowMs + timeoutMs`），交给主程序自己的定时器驱动；
    /// - `nowMs` 由调用方给（steady clock 的毫秒数），库不读时钟。
    int64_t begin(std::string_view op, std::string_view owner = {}, int32_t timeoutMs = 0,
                  int64_t nowMs = 0) {
        RequestEntry entry;
        const int64_t id = next_.fetch_add(1, std::memory_order_relaxed);
        entry.id        = id;
        entry.op        = std::string{op};
        entry.owner     = std::string{owner};
        entry.startedMs = nowMs;
        if (timeoutMs > 0) {
            entry.deadlineMs = nowMs + timeoutMs;
        }
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (entry.deadlineMs > 0) {
                deadlines_[entry.id] = entry.deadlineMs;
            }
            entries_[id] = std::move(entry);
        }
        return id;
    }

    /// 把请求标成"属于某个 owner"（卸载时按 owner 批量结算）
    void setOwner(int64_t id, std::string_view owner) {
        std::lock_guard<std::mutex> lock{mutex_};
        const auto                  it = entries_.find(id);
        if (it != entries_.end()) {
            it->second.owner = std::string{owner};
        }
    }

    /// 内部请求的终局交给外层请求（`plugin.reload` 这类一次调用内部拆成几步时用）
    ///
    /// 内层请求不再单独出结果；它的归属跟着外层走（清空内层 owner）与到期时刻。
    bool alias(int64_t innerId, int64_t outerId) {
        if (innerId <= 0 || outerId <= 0 || innerId == outerId) {
            return false;
        }
        std::lock_guard<std::mutex> lock{mutex_};
        const auto                  it = entries_.find(innerId);
        if (it == entries_.end()) {
            return false;
        }
        aliases_[innerId] = outerId;
        it->second.owner.clear();
        deadlines_.erase(innerId);
        it->second.deadlineMs = 0;
        return true;
    }

    /// 结算一次请求（**幂等**：已经有终局时返回 false，调用方不再补结果）
    ///
    /// 解析过别名时 `out->id` 是外层请求号，`out->op` 是外层请求的操作名。
    bool settle(int64_t id, RequestEntry* out = nullptr) {
        std::lock_guard<std::mutex> lock{mutex_};
        if (const auto alias = aliases_.find(id); alias != aliases_.end()) {
            id = alias->second;
            aliases_.erase(alias);
        }
        const auto it = entries_.find(id);
        if (it == entries_.end()) {
            return false;
        }
        if (out) {
            *out = it->second;
        }
        deadlines_.erase(id);
        entries_.erase(it);
        return true;
    }

    /// 取消（语义同 [settle]：请求不存在返回 false，调用方按"已经收尾过"处理即可）
    bool cancel(int64_t id, RequestEntry* out = nullptr) { return settle(id, out); }

    /// 全部还没收尾的请求（停机用）；`exceptId` 是正在做停机的那条请求自己
    std::vector<RequestEntry> failAll(int64_t exceptId = 0) {
        std::lock_guard<std::mutex> lock{mutex_};
        std::vector<RequestEntry>   hits;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->first == exceptId) {
                ++it;
                continue;
            }
            hits.push_back(it->second);
            it = entries_.erase(it);
        }
        deadlines_.clear();
        aliases_.clear();
        return hits;
    }

    /// 某个 owner 名下还没收尾的请求（插件卸载 / 会话结束用）
    ///
    /// `exceptId` 是"正在做卸载的那条请求"自己：它也在 owner 名下，不能被自己结算，
    /// 否则调用方拿到的是 `plugin_unloaded` 而不是卸载结论。
    std::vector<RequestEntry> failAllOf(std::string_view owner, int64_t exceptId = 0) {
        return failAllWhere([owner](const RequestEntry& entry) { return entry.owner == owner; },
                            exceptId);
    }

    /// 按自定义条件批量结算（主程序用自己的归属规则时用，例如"实例名去掉前缀后比 id"）
    std::vector<RequestEntry>
    failAllWhere(const std::function<bool(const RequestEntry&)>& predicate, int64_t exceptId = 0) {
        std::lock_guard<std::mutex> lock{mutex_};
        std::vector<RequestEntry>   hits;
        if (!predicate) {
            return hits;
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->first != exceptId && predicate(it->second)) {
                hits.push_back(it->second);
                deadlines_.erase(it->first);
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        return hits;
    }

    bool contains(int64_t id) const {
        std::lock_guard<std::mutex> lock{mutex_};
        return entries_.find(id) != entries_.end();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return entries_.size();
    }

    /// 全部请求里的最早到期时刻（`0` = 没有带超时的请求）
    ///
    /// 主程序用自己的定时器等到这个时刻，再调 [popExpired] 结算。"库内不放定时器"的原因：
    /// 两边主程序各有自己的事件循环，库替谁决定线程都不对。
    int64_t nextDeadlineMs(int64_t nowMs) const {
        std::lock_guard<std::mutex> lock{mutex_};
        int64_t                     next = 0;
        for (const auto& [id, deadline] : deadlines_) {
            (void)id;
            if (deadline <= nowMs) {
                return nowMs; ///< 已经到期：让调用方立刻跑一次 popExpired
            }
            next = (next == 0 || deadline < next) ? deadline : next;
        }
        return next;
    }

    /// 结算所有已到期的请求（调用方拿到条目自己发结果事件）
    std::vector<RequestEntry> popExpired(int64_t nowMs) {
        std::lock_guard<std::mutex> lock{mutex_};
        std::vector<RequestEntry>   hits;
        for (auto it = deadlines_.begin(); it != deadlines_.end();) {
            if (it->second > nowMs) {
                ++it;
                continue;
            }
            const auto entry = entries_.find(it->first);
            if (entry != entries_.end()) {
                hits.push_back(entry->second);
                entries_.erase(entry);
            }
            it = deadlines_.erase(it);
        }
        return hits;
    }

private:
    mutable std::mutex              mutex_;
    std::map<int64_t, RequestEntry> entries_;
    std::map<int64_t, int64_t>      aliases_;
    std::map<int64_t, int64_t>      deadlines_;
    std::atomic<int64_t>            next_{1};
};

/// ==================== 跨堆字符串 ====================
///
/// 两端可能各有一套 CRT：分配与释放**都用调用方给的分配器**，库不替调用方决定用哪个堆。
using AllocFn = void* (*)(size_t);
using FreeFn  = void (*)(void*);

/// 把一段文本复制成 `PluginxxString`（用调用方的分配器）；成功返回 `PLUGINXX_RESULT_OK`
inline int32_t strdupTo(std::string_view src, PluginxxString* out, AllocFn alloc) {
    if (!out) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    out->data = nullptr;
    out->size = 0;
    if (src.empty()) {
        return PLUGINXX_RESULT_OK;
    }
    if (!alloc) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    void* memory = alloc(src.size() + 1);
    if (!memory) {
        return PLUGINXX_RESULT_ERR_NO_MEMORY;
    }
    std::memcpy(memory, src.data(), src.size());
    static_cast<char*>(memory)[src.size()] = '\0';
    out->data = static_cast<char*>(memory);
    out->size = static_cast<uint64_t>(src.size());
    return PLUGINXX_RESULT_OK;
}

/// 释放 [strdupTo] 分配的字符串（传 `nullptr` 安全）
inline void freeString(PluginxxString* str, FreeFn freeFn) {
    if (!str || !str->data) {
        return;
    }
    if (freeFn) {
        freeFn(str->data);
    }
    str->data = nullptr;
    str->size = 0;
}

} // namespace ffi
} // namespace pluginxx

#endif /* PLUGINXX_HOST_FFI_BRIDGE_H */
