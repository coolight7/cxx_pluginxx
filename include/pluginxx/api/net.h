/// 插件网络能力接口表 (`<应用名>.net`) 与结果码
///
/// 归属: 这张表**只依赖"op + 参数 JSON → 结果 JSON"这套通用写法**, 不需要主程序业务语义,
/// 所以放在通用表这一侧 (与 `capabilities` / `broadcasts` 同样由应用名拼出 IID)。
/// 实现在 `cxx_utilxx` 的 `utilxx::PluginNetHub` (HTTP 客户端 / HTTP 服务端 / TCP /
/// WebSocket / UDP, 自有网络线程), 主程序只负责把表接到宿主与插件实例上。
///
/// 三条硬约定:
/// - 服务端默认**只监听回环** (`host` 缺省 `127.0.0.1`); 写 `0.0.0.0` 表示同网段可访问,
///   由插件自己承担暴露风险;
/// - 网络能力跟着**插件实例**: 停用 / 卸载 / 框架停机时, 该插件的全部服务与连接一起关掉;
/// - 事件走广播: 频道 `plugin.<插件 id>.net`, 载荷带 `kind` 字段。
///
/// 操作 (`call` / `call_async` 的 `op` 参数; C++ 常量见 `pluginxx/net/ops.h`):
///
/// | op | 形态 | 主要参数 | 事件 / 说明 |
/// |---|---|---|---|
/// | `http.request` | 同步受理 + 事件 / 异步 | url, method?, headers?, body?, bodyBase64?, json?, timeoutMs?, maxBytes?, followRedirects?, maxRedirects? | 结果 `http.response` |
/// | `http.fetch` | 异步 | 同 `http.request` | 只拿最终结果, 不收中间事件 |
/// | `http.download` | 异步 | url, path?, ... | 落盘到插件数据目录, 带进度事件 |
/// | `http.cancel` | 同步 | requestId | 取消进行中的请求 |
/// | `http.serve` / `http.route` / `http.unroute` / `http.stop` / `http.info` | 同步 | id?, host?, port?, path?, ... | `http.listening` / `http.request` |
/// | `http.respond` | 同步 | requestId, status?, headers?, body?, bodyBase64?, contentType? | 回答一条 `http.request` |
/// | `socket.connect` / `socket.listen` / `socket.send` / `socket.close` / `socket.serverStop` | 同步 (受理) | id?, host, port, ... | `socket.connected` / `socket.data` / `socket.closed` / `socket.accepted` |
/// | `ws.connect` / `ws.listen` / `ws.send` / `ws.ping` / `ws.close` / `ws.serverStop` | 同步 (受理) | id?, url? 或 host/port/path, ... | `ws.open` / `ws.message` / `ws.close` / `ws.listening` |
/// | `udp.open` / `udp.send` / `udp.join` / `udp.leave` / `udp.close` / `udp.info` | 同步 | id?, host?, port?, multicast?, ... | 广播与组播 (局域网发现) |
/// | `net.info` | 同步 | 无 | 服务 / 连接 / 计数器快照 |
///
/// 结果 JSON 一律 `{ok:true,...}` 或 `{ok:false,error,message}`; 传输失败只体现在结果里
/// (`call` 返回 [PLUGINXX_NET_OK] = "操作执行完毕"), 表入口只在参数/状态层面失败时才返回负值。
#ifndef PLUGINXX_API_NET_H
#define PLUGINXX_API_NET_H

#include "pluginxx/api/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

/* ==================== 结果码 (表入口的返回值; 业务结论在结果 JSON 的 ok 里) ==================== */

#define PLUGINXX_NET_OK            0  ///< 操作执行完毕 (业务结论看结果 JSON)
#define PLUGINXX_NET_ERR_ARG      -1  ///< 参数非法 (缺字段 / 类型不对)
#define PLUGINXX_NET_ERR_STATE    -2  ///< 当前状态不允许 (网络能力没启动)
#define PLUGINXX_NET_ERR_JSON     -3  ///< 参数不是合法 JSON
#define PLUGINXX_NET_ERR_NOT_FOUND -4 ///< 请求 / 连接 / 服务不存在 (取消已完成的操作)
#define PLUGINXX_NET_ERR_TIMEOUT  -5  ///< 等网络线程超时
#define PLUGINXX_NET_ERR_PERMISSION -6 ///< 没有权限 (例如落盘路径越界)
#define PLUGINXX_NET_ERR_QUEUE_FULL -7 ///< 排队已满 (服务 / 连接 / 请求上限)
#define PLUGINXX_NET_ERR_NO_MEMORY -8 ///< 内存不足
#define PLUGINXX_NET_ERR_INTERNAL -99 ///< 实现内部错误

/* ==================== 接口表: 网络 (<应用名>.net) ==================== */

#define PLUGINXX_IFACE_NET PLUGINXX_APP_ID("net")
/// v1: 同步 call / 异步 call_async / cancel
#define PLUGINXX_IFACE_NET_VERSION     1
#define PLUGINXX_IFACE_NET_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define PLUGINXX_IFACE_NET_BASELINE_SIZE sizeof(PluginxxNetIface)

typedef struct PluginxxNetIface {
    int32_t  version;     ///< 主程序填 [PLUGINXX_IFACE_NET_VERSION]
    uint32_t struct_size;

    /// 同步操作: op + 参数 JSON → 结果 JSON (out_json 由插件框架分配, 调用方释放)
    ///
    /// 返回 [PLUGINXX_NET_OK] 表示操作执行完毕 (业务结论在结果 JSON 的 `ok` 里);
    /// 参数非法 / 未启动 / JSON 非法等情况返回负的结果码, 不写 out_json。
    /// `http.request` 走这里只**受理** (结果经 `http.response` 事件或 `call_async` 的
    /// 完成通知给出)。
    int32_t(PLUGINXX_CALL* call)(
        const PluginxxHost*       host,
        const PluginxxStringView* op,
        const PluginxxStringView* args_json,
        PluginxxString*           out_json
    );

    /// 异步操作 (`http.request` / `http.fetch` / `http.download`): 受理成功写出
    /// request_id, 结果**恰好一次**经 `notify->done` 给出 (插件框架线程);
    /// 受理失败返回负的结果码、写 out_json 说明, **不回调**。
    int32_t(PLUGINXX_CALL* call_async)(
        const PluginxxHost*           host,
        const PluginxxStringView*     op,
        const PluginxxStringView*     args_json,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               out_json,
        int64_t*                      out_request_id
    );

    /// 取消一个进行中的异步请求 (不存在 / 已完成返回 [PLUGINXX_NET_ERR_NOT_FOUND])
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
