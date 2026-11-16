/// pluginxx 表调用的主程序侧执行框架 (主程序内部, 非 ABI)
///
/// 背景: 接口表 (表头里的 `<应用名>.*` 与主程序业务表) 一律是**异步操作契约** ——
/// 成员返回 `PluginxxOperatorHandle*` 表示"已受理", 结果经 `PluginxxOperatorCallback`
/// 恰好一次交回; 返回 `NULL` 表示"未受理" (参数错 / 作用域失效 / 实例正在关闭),
/// 此时写入 `error_out`, 且 `cb` 不会被调用。
///
/// 本头把这套契约落在主程序一侧的实现收敛到一处:
/// - [startTableOp]: 受理 + 在自己的 IO 线程执行工作体 + 交回结果;
/// - 参数/作用域校验仍由各表入口自己做 (同步返回 `NULL` + `error_out`);
/// - 工作体只写"业务", 不再关心线程投递与完成协议。
///
/// 线程与顺序:
/// - 工作体在自己的 IO 线程执行; 调用方本来就在 IO 线程时**内联执行**, 因此
///   "注册完立刻可见"的顺序与同步调用完全一致, 也省掉一次投递;
/// - 其它线程调用时按受理顺序排队 (FIFO), 插件随后 `co_await` 完成即可。
///
/// 为什么要内联: 表成员绝大多数只是查表或做一次小序列化, 跨线程往返的固定开销
/// (两次投递 + 两次唤醒) 比本体大 2~3 个数量级; 内联只在调用方**已经**在 IO 线程
/// 时发生, 不产生任何等待, 所以不会回到"同步阻塞"的老问题。
#ifndef PLUGINXX_HOST_TABLE_OP_H
#define PLUGINXX_HOST_TABLE_OP_H

#include "pluginxx/api/abi.h"
#include "pluginxx/runtime/op_driver.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace pluginxx {

/// 执行期失败 (工作体统一用它表达 "这次没能给出结果")
///
/// 表入口捕获后按 `PLUGINXX_OPERATOR_FAILED` 交回, 正文就是异常文本; 不写
/// `[Error: ...]` 一类前缀 (那是工具结果给模型的排版, 与本协议无关)。
class TableOpError : public std::runtime_error {
public:

    explicit TableOpError(std::string message) :
        std::runtime_error(std::move(message)) {}
};

/// 受理阶段的失败 (参数错 / 作用域失效 / 实例不可用): 写入 `error_out` 并返回 NULL
///
/// 各表入口自己决定用什么规则判定, 这里只负责把原因复制到主程序堆字符串。
inline void setTableOpError(PluginxxString* error_out, std::string_view message) {
    if (!error_out) {
        return;
    }
    hostMemorySetString(error_out, std::string{message});
}

/// 启动一次表操作 (**任意线程**)
///
/// - `mgr`: 拥有 IO executor 的管理器 (`isIoThread()` / `postToIo()` / `runtime()`);
/// - `cb` / `ud`: 完成回调 (可空 = 只受理不回程);
/// - `error_out`: 未受理的原因 (可空);
/// - `label`: 诊断名 (出现在未终结操作摘要与日志里, 形如 `session.get_messages`);
/// - `work`: 工作体, 返回结果正文; 抛 [TableOpError] / 任何标准异常 = 本次失败。
///
/// 回调时机:
/// - **调用方本来就在自己的 IO 线程**: 工作体当场执行, `cb` **在本次调用返回之前**
///   (同一个栈上) 被调用一次 —— 注册类调用"登记后立刻可见"的语义与同步调用一致,
///   插件也不必为自己的线程多等一次 executor 轮次;
/// - **其它线程**: 工作体排队到 IO 线程执行, `cb` 在 IO 线程发出 (不内联)。
///
/// 两种情况下 `cb` 都恰好一次, 且都在 IO 线程上。
///
/// 返回非 NULL = 已受理 (结果经 `cb` 恰好一次); 返回 NULL = 未受理 (已写 `error_out`)。
template<typename Mgr, typename Fn>
PluginxxOperatorHandle* startTableOp(
    Mgr*                     mgr,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work
) noexcept {
    if (!mgr) {
        setTableOpError(error_out, "plugin runtime is unavailable");
        return nullptr;
    }
    std::shared_ptr<OpCore> core;
    bool                    inlineIo = false;
    try {
        const auto& runtime = mgr->runtime();
        if (!runtime || !runtime->executor) {
            setTableOpError(error_out, "plugin runtime has no IO executor");
            return nullptr;
        }
        inlineIo = mgr->isIoThread();
        core     = OpCore::createDetached(runtime, label);
        if (!inlineIo) {
            core->setCallback(cb, ud);
        }
        core->accept();
    } catch (const std::exception& e) {
        setTableOpError(error_out, e.what());
        return nullptr;
    } catch (...) {
        setTableOpError(error_out, "plugin operation could not be accepted");
        return nullptr;
    }

    auto run = [core, cb, ud, inlineIo, work = std::forward<Fn>(work)]() mutable {
        int32_t     status = PLUGINXX_OPERATOR_OK;
        std::string payload;
        try {
            payload = work();
        } catch (const std::exception& e) {
            status  = PLUGINXX_OPERATOR_FAILED;
            payload = e.what();
        } catch (...) {
            status  = PLUGINXX_OPERATOR_FAILED;
            payload = "plugin table operation failed unexpectedly";
        }
        const PluginxxStringView view{payload.data(), static_cast<uint64_t>(payload.size())};
        // 内联执行 (调用方本来就在 IO 线程) 时**当场**回调: 注册类调用"登记后立刻可见"
        // 的语义与同步调用完全一致, 插件也不必为自己的线程多等一次 executor 轮次。
        // 书的记账仍交给 OpCore 的完成通道 (只是它不再持有回调)。
        if (inlineIo && cb) {
            try {
                cb(ud, status, &view);
            } catch (...) {
                XX_LOGW("plugin table call `{}` completion callback threw", core->label());
            }
        }
        OpCore::onDoneCore(core.get(), status, &view);
    };
    try {
        mgr->postToIo(std::move(run));
    } catch (const std::exception& e) {
        core->reject();
        setTableOpError(error_out, e.what());
        return nullptr;
    } catch (...) {
        core->reject();
        setTableOpError(error_out, "plugin runtime IO executor is stopped");
        return nullptr;
    }
    return core->handle();
}

/// [startTableOp] 的便捷形态: 工作体没有结果正文 (只关心成功/失败)
template<typename Mgr, typename Fn>
PluginxxOperatorHandle* startTableOpVoid(
    Mgr*                     mgr,
    PluginxxOperatorCallback cb,
    void*                    ud,
    PluginxxString*          error_out,
    std::string              label,
    Fn&&                     work
) noexcept {
    return startTableOp(
        mgr,
        cb,
        ud,
        error_out,
        std::move(label),
        [work = std::forward<Fn>(work)]() mutable -> std::string {
            work();
            return std::string{};
        }
    );
}

} // namespace pluginxx

#endif /* PLUGINXX_HOST_TABLE_OP_H */
