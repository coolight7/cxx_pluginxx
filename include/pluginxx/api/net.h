/// 插件网络能力接口表 (`<应用名>.net`) 与结果码
///
/// 归属: 这张表**只依赖"op + 参数 JSON → 结果 JSON"这套通用写法**, 不需要主程序业务语义,
/// 所以放在通用表这一侧 (与 `capabilities` / `broadcasts` 同样由应用名拼出 IID)。
/// 实现分两层: 通用网络服务在 `cxx_utilxx` 的 `utilxx::NetHub` (HTTP 客户端 / HTTP 服务端 /
/// TCP / WebSocket / UDP, 自有网络线程, 按属主 id 隔离资源), 主程序负责把这张表接到
/// 插件实例 (`<插件 id>` = 属主 id)、并把事件发到广播频道上。
///
/// 三条硬约定:
/// - 服务端默认**只监听回环** (`host` 缺省 `127.0.0.1`); 写 `0.0.0.0` 表示同网段可访问,
///   由插件自己承担暴露风险;
/// - 网络能力跟着**插件实例**: 停用 / 卸载 / 框架停机时, 该插件的全部服务与连接一起关掉;
/// - 事件走广播: 频道 `plugin.<插件 id>.net`, 载荷带 `kind` 字段。
///
/// 操作 (`call` 的 `op` 参数; C++ 常量见 `pluginxx/net/ops.h`):
///
/// | op | 结果形态 | 主要参数 | 事件 / 说明 |
/// |---|---|---|---|
/// | `http.request` | 完成回调 (受理 JSON) + 事件 | url, method?, headers?, body?, bodyBase64?, json?, timeoutMs?, maxBytes?, followRedirects?, maxRedirects? | 结果 `http.response` |
/// | `http.fetch` | 完成回调 (最终结果) | 同 `http.request` | 只拿最终结果, 不收中间事件 |
/// | `http.download` | 完成回调 (最终结果) | 同 `http.request` | 落盘到插件数据目录, 带进度事件 |
/// | `http.cancel` | 完成回调 (立即) | requestId | 取消进行中的请求 |
/// | `http.serve` / `http.route` / `http.unroute` / `http.stop` / `http.info` | 完成回调 (立即) | id?, host?, port?, path?, ... | `http.listening` / `http.request` |
/// | `http.respond` | 完成回调 (立即) | requestId, status?, headers?, body?, bodyBase64?, contentType? | 回答一条 `http.request` |
/// | `socket.connect` / `socket.listen` / `socket.send` / `socket.close` / `socket.serverStop` | 完成回调 (立即) | id?, host, port, ... | `socket.connected` / `socket.data` / `socket.closed` / `socket.accepted` |
/// | `ws.connect` / `ws.listen` / `ws.send` / `ws.ping` / `ws.close` / `ws.serverStop` | 完成回调 (立即) | id?, url? 或 host/port/path, ... | `ws.open` / `ws.message` / `ws.close` / `ws.listening` |
/// | `udp.open` / `udp.send` / `udp.join` / `udp.leave` / `udp.close` / `udp.info` | 完成回调 (立即) | id?, host?, port?, multicast?, ... | 广播与组播 (局域网发现) |
/// | `net.info` | 完成回调 (快照) | 无 | 服务 / 连接 / 计数器快照 |
///
/// 结果 JSON 一律 `{ok:true,...}` 或 `{ok:false,error,message}`; 传输失败只体现在结果里
/// (完成回调 `status = PLUGINXX_OPERATOR_OK` = "操作跑完了"), 表入口只在参数 / 状态层面
/// 失败时返回 `NULL` (未受理)。
///
/// **没有同步版本**: `call` 是唯一入口。想"提交完就不管"的调用方传 `cb = NULL`
/// (动作照常执行, 只是不回收结果); 要等结果请用协程 —— 不要在主程序 IO 线程上自旋等待,
/// 一条慢请求会拖住整个主程序。
#ifndef PLUGINXX_API_NET_H
#define PLUGINXX_API_NET_H

#include "pluginxx/api/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

/* ==================== 结果码 (表入口的返回值; 业务结论在结果 JSON 的 ok 里) ==================== */
//
// 码表只有一份，在 `pluginxx/api/result.h`（通用段 / 广播段 / 生命周期与内部段都在那里）；
// 网络段（-100 ~ -199）属于实现网络的 utilxx（`utilxx/net_error.h`）。

#include "pluginxx/api/result.h"

/* ==================== 接口表: 网络 (<应用名>.net) ==================== */

#define PLUGINXX_IFACE_NET PLUGINXX_APP_ID("net")
/// v1: 同步 call / 异步 call_async / cancel
/// v2: `call_async` 删掉, `call` 本身改成异步操作契约 (`request_id` 变成出参, 结果经
///     完成回调交回); `cancel` 不变 (投递即返回, 没有结果)。基线随本次布局变化重置
#define PLUGINXX_IFACE_NET_VERSION     2
#define PLUGINXX_IFACE_NET_MIN_VERSION 2
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_NET_BASELINE_SIZE sizeof(PluginxxNetIface)

typedef struct PluginxxNetIface {
    int32_t  version;     ///< 主程序填 [PLUGINXX_IFACE_NET_VERSION]
    uint32_t struct_size;

    /// 一次网络操作 (**任意线程**; 网络服务自有线程, 不会占用主程序 IO 线程)
    ///
    /// - 返回非 NULL = 已受理, 结果**恰好一次**经 `cb` 交回 (`cb = NULL` 时只执行不回程);
    ///   完成时 `payload` 是结果 JSON (`{ok:true,...}` / `{ok:false,error,message}`),
    ///   `status = PLUGINXX_OPERATOR_FAILED` 表示执行期失败 (正文是原因);
    /// - 返回 NULL = 未受理 (参数错 / 实例不可用 / 网络服务未启动), 原因写 `error_out`,
    ///   此时 `cb` 不会被调用;
    /// - `request_id_out` 非空时写出本次请求 id (取消用; 立即完成的 op 也会有一个 id)。
    PluginxxOperatorHandle*(PLUGINXX_CALL* call)(
        const PluginxxHost*       host,
        const PluginxxStringView* op,
        const PluginxxStringView* args_json,
        int64_t*                  request_id_out,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// 取消一个进行中的请求 (不存在 / 已完成返回 [PLUGINXX_RESULT_ERR_NOT_FOUND])
    ///
    /// 投递即返回: 取消是通知式的, 结果不看这里。
    int32_t(PLUGINXX_CALL* cancel)(
        const PluginxxHost* host,
        int64_t             request_id,
        PluginxxString*     out_json
    );
} PluginxxNetIface;

#pragma pack(pop)

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* PLUGINXX_API_NET_H */
