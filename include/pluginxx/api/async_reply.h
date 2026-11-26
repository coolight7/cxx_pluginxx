/// 一处的"恰好完成一次"结果槽: 等待侧是协程, 完成侧可以是任意线程
///
/// 用途: 插件处理器按操作契约受理 (返回句柄) 并在结束时调用一次完成通知。
/// 主程序在插件执行线程上 `co_await` 这个槽, 完成通知可能来自插件自己的线程
/// (网络回调 / 脚本引擎 / 定时器), 因此唤醒必须投递回等待方的执行器,
/// 不能在完成侧的线程上直接恢复协程 (执行器不是那个线程的)。
///
/// 用法:
/// ```cpp
/// auto reply = pluginxx::AsyncReply<std::string>::create(executor);
/// notify->done(...) 里调 reply->set(text);   // 任意线程, 只认第一次
/// std::string text = co_await reply->wait(); // 插件执行线程
/// ```
///
/// 约定:
/// - `set` 只认第一次 (后来的丢弃), 与处理器"恰好回调一次"的契约一致;
/// - `set` 可以发生在 `wait` 之前 (立即完成的处理器) —— 这时 `wait` 不再等;
/// - 取消 (插件卸载 / 主程序停机) 用 [cancel] 收尾, 等待方拿到 [canceled] 标记的默认值。
#pragma once

/// 来源是 Boost.Asio (本工程不引入 standalone asio): 用 `boost/asio/...` 写完整路径,
/// 再给一个与 `pluginxx/kit/kit.h` 一致的全局别名
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace asio = ::boost::asio;

namespace pluginxx {

template <typename T> class AsyncReply final
    : public std::enable_shared_from_this<AsyncReply<T>> {
public:
  /// 建一个结果槽 (执行器 = 等待方所在的那个, 唤醒会投递到它上面)
  static std::shared_ptr<AsyncReply<T>> create(asio::any_io_executor ex) {
    auto reply = std::shared_ptr<AsyncReply<T>>(new AsyncReply<T>(std::move(ex)));
    return reply;
  }

  /// 完成一次 (任意线程; 只认第一次)
  void set(T value) {
    bool wake = false;
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (done_) {
        return;
      }
      done_ = true;
      value_ = std::move(value);
      wake = waiting_;
    }
    if (wake) {
      wakeWaiter();
    }
  }

  /// 取消 (任意线程; 用默认值完成, 与 [set] 同样只认第一次)
  ///
  /// 取消后 [canceledOf] 为真: 调用方据此把"被打断"与"插件答了空"分开。
  void cancel() {
    bool wake = false;
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (done_) {
        return;
      }
      done_ = true;
      canceled_ = true;
      wake = waiting_;
    }
    if (wake) {
      wakeWaiter();
    }
  }

  /// 已经完成 (含取消)
  bool ready() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return done_;
  }

  /// 是否被取消
  bool canceled() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return canceled_;
  }

  /// 等结果 (只能在等待方的执行器上 `co_await`)
  ///
  /// - 已经完成时立刻返回, 不碰定时器;
  /// - 否则把定时器设置到"很久以后", 由 [set] / [cancel] 把它提前;
  ///   提前用的是 `expires_at(过去)`, 而不是 `cancel()` —— 定时器可能还没开始等
  ///   (`async_wait` 尚未发起), 那时 `cancel()` 不生效会让等待方永远等下去。
  asio::awaitable<T> wait() {
    bool needWait = false;
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (!done_) {
        waiting_ = true;
        needWait = true;
      }
    }
    if (needWait) {
      (void)co_await timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }
    std::lock_guard<std::mutex> lock{mutex_};
    co_return value_;
  }

private:
  explicit AsyncReply(asio::any_io_executor ex) : timer_(std::move(ex)) {
    // 默认不到点: 只有 [set] / [cancel] 会把它提前
    timer_.expires_at(std::chrono::steady_clock::time_point::max());
  }

  /// 把等待方的定时器提前到"现在" (投递到定时器自己的执行器上做, 定时器不是线程安全的)
  void wakeWaiter() {
    auto self = this->shared_from_this();
    asio::post(timer_.get_executor(), [self] {
      self->timer_.expires_at(std::chrono::steady_clock::time_point::min());
    });
  }

  mutable std::mutex mutex_;
  bool done_ = false;
  bool canceled_ = false;
  bool waiting_ = false;
  T value_{};
  asio::steady_timer timer_;
};

/// 已经完成的结果 (给"处理器立刻就有结果"的写法省掉协程骨架)
///
/// ```cpp
/// co_return pluginxx::readyValue(R"({"ok":true})");
/// ```
inline asio::awaitable<std::string> readyValue(std::string text) {
  co_return text;
}

} // namespace pluginxx
