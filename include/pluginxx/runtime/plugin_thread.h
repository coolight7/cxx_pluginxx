/// 插件执行线程 (一条 io_context + 一条工作线程)
///
/// 定位: 宿主 (主程序) 想把"插件代码"与自己的 io 线程分开时, 需要有一条专跑插件
/// 回调的工作线程。本类把这件事收敛到一处: 建立 io_context、持有 work guard、
/// 起线程、停机时 join。
///
/// 用法 (顺序很重要):
/// 1. 宿主在创建插件管理器**之前**声明本对象并 `start("agentxx-plugin")`;
/// 2. 把 `executor()` 交给管理器 (`PluginManagerBase::setPluginExecutor`);
/// 3. 成员声明顺序: 本对象在管理器之前 —— 逆序析构时管理器先走, 线程后停。
///    管理器析构时已经 `shutdownAll()`/`shutdownAsync()`, 不会再投递新派发项。
///
/// 线程约定:
/// - `start` / `stop` 只在初始化与停机阶段各调一次, 不做线程安全保护;
/// - `executor()` 可从任意线程取 (只读 `shared_ptr`), 但必须在 `start` 之后。
#pragma once

#include "asio/any_io_executor.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "utilxx_base/log.h"

#include <memory>
#include <string>
#include <string_view>
#include <thread>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

namespace pluginxx {

/// 给当前线程起一个便于在日志/调试器里区分的名字 (平台不支持时为空操作)
inline void setCurrentThreadName(std::string_view name) noexcept {
    if (name.empty()) {
        return;
    }
#if XX_IS_WIN_D
    try {
        const std::wstring wide{name.begin(), name.end()};
        ::SetThreadDescription(::GetCurrentThread(), wide.c_str());
    } catch (...) {
    }
#elif defined(__linux__)
    // pthread 限制 16 字节 (含结尾 NUL)
    std::string shortName{name};
    if (shortName.size() > 15) {
        shortName.resize(15);
    }
    ::pthread_setname_np(::pthread_self(), shortName.c_str());
#elif defined(__APPLE__)
    std::string shortName{name};
    if (shortName.size() > 15) {
        shortName.resize(15);
    }
    ::pthread_setname_np(shortName.c_str());
#else
    (void)name;
#endif
}

/// 一条专跑插件代码的工作线程
class PluginExecutorThread {
public:

    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    PluginExecutorThread() = default;

    ~PluginExecutorThread() {
        stop();
        // io_context 只在对象销毁时才释放: 宿主可能仍在别处存着这个执行器的副本
        // (管理器里的 `PluginRuntime::pluginExecutor`), 提前释放会让那份副本变成
        // 悬垂执行器 —— 停线程后再派发就会读到已释放的 io_context。
        context_.reset();
    }

    PluginExecutorThread(const PluginExecutorThread&)            = delete;
    PluginExecutorThread& operator=(const PluginExecutorThread&) = delete;

    /// 启动线程 (幂等)。失败返回 false (调用方按"没有插件线程"继续, 插件代码回到 io)
    bool start(std::string_view name = "plugin") {
        if (context_) {
            return true;
        }
        try {
            auto        context = std::make_shared<asio::io_context>();
            auto        work    = std::make_unique<WorkGuard>(asio::make_work_guard(*context));
            std::string tag{name};
            // 线程持有 context 的强引用: 即使宿主在 run() 期间销毁本对象, io_context
            // 也活到 run() 返回, 不会出现"线程用已释放的 context"。
            std::thread thread([context, tag] {
                setCurrentThreadName(tag);
                try {
                    context->run();
                } catch (const std::exception& e) {
                    XX_LOGW("Plugin executor thread `{}` stopped with error: {}", tag, e.what());
                } catch (...) {
                    XX_LOGW("Plugin executor thread `{}` stopped with unknown error", tag);
                }
            });
            context_ = std::move(context);
            work_    = std::move(work);
            thread_  = std::move(thread);
            name_    = std::move(tag);
            return true;
        } catch (const std::exception& e) {
            XX_LOGE("Plugin executor thread could not start: {}", e.what());
            context_.reset();
            work_.reset();
            return false;
        } catch (...) {
            XX_LOGE("Plugin executor thread could not start");
            context_.reset();
            work_.reset();
            return false;
        }
    }

    /// 停止并等待线程退出 (幂等); 已经 stop 过时是空操作
    ///
    /// **必须在插件管理器析构之后调用**: 线程停了之后再投递派发项, 那些回调会
    /// 留在待重放队列里永不执行 (实例关闭最终按超时收尾)。
    void stop() noexcept {
        if (thread_.joinable()) {
            // 先放掉 work guard 让 run() 自然返回, 再 stop() 兜底 (有未完成的定时器时
            // run() 不会自己返回)。
            work_.reset();
            if (context_) {
                context_->stop();
            }
            if (thread_.get_id() == std::this_thread::get_id()) {
                // 宿主对象从插件线程自身销毁 (最后一个引用在回调里释放): 不能 join 自己。
                // 线程已停止, 会自行退出; io_context 由线程闭包里的强引用保活到 run() 返回。
                thread_.detach();
            } else {
                thread_.join();
            }
        }
        // 不释放 context_: 交给管理器的执行器副本仍指向它, 此时派发看到"已停止"
        // 就被丢弃; 提前释放会让那份副本变成悬垂执行器。
        name_.clear();
    }

    /// 未 start / 已 stop 时为空 executor
    asio::any_io_executor executor() const noexcept {
        if (!context_) {
            return {};
        }
        return asio::any_io_executor{context_->get_executor()};
    }

    bool running() const noexcept {
        return thread_.joinable();
    }

    const std::string& name() const noexcept {
        return name_;
    }

private:

    std::shared_ptr<asio::io_context> context_;
    std::unique_ptr<WorkGuard>        work_;
    std::thread                       thread_;
    std::string                       name_;
};

} // namespace pluginxx
