/// 插件网络能力的操作名 (C++ 常量)
///
/// 这些字符串就是 [`pluginxx/api/net.h`](../api/net.h) 里 `call` / `call_async` 的 `op`
/// 参数, 表契约本身只认字符串 —— 本头只是给 C++ 使用方一份拼写正确的常量, 不参与 ABI。
///
/// 约定: 同步操作的结果 JSON 里带 `ok`; `http.request` 可以"同步受理 + 听事件",
/// 也可以走 `call_async` 拿完成回调; `http.fetch` / `http.download` 是同一个 HTTP 实现的
/// 两个常用形态, 只走 `call_async`。
#ifndef PLUGINXX_NET_OPS_H
#define PLUGINXX_NET_OPS_H

namespace pluginxx {
namespace net_op {

inline constexpr const char* httpRequest      = "http.request";
inline constexpr const char* httpFetch        = "http.fetch";
inline constexpr const char* httpDownload     = "http.download";
inline constexpr const char* httpCancel       = "http.cancel";
inline constexpr const char* httpServe        = "http.serve";
inline constexpr const char* httpRoute        = "http.route";
inline constexpr const char* httpUnroute      = "http.unroute";
inline constexpr const char* httpRespond      = "http.respond";
inline constexpr const char* httpStop         = "http.stop";
inline constexpr const char* httpInfo         = "http.info";
inline constexpr const char* socketConnect    = "socket.connect";
inline constexpr const char* socketListen     = "socket.listen";
inline constexpr const char* socketSend       = "socket.send";
inline constexpr const char* socketClose      = "socket.close";
inline constexpr const char* socketServerStop = "socket.serverStop";
inline constexpr const char* wsConnect        = "ws.connect";
inline constexpr const char* wsListen         = "ws.listen";
inline constexpr const char* wsSend           = "ws.send";
inline constexpr const char* wsPing           = "ws.ping";
inline constexpr const char* wsClose          = "ws.close";
inline constexpr const char* wsServerStop     = "ws.serverStop";
inline constexpr const char* udpOpen          = "udp.open";
inline constexpr const char* udpSend          = "udp.send";
inline constexpr const char* udpJoin          = "udp.join";
inline constexpr const char* udpLeave         = "udp.leave";
inline constexpr const char* udpClose         = "udp.close";
inline constexpr const char* udpInfo          = "udp.info";
inline constexpr const char* info             = "net.info";

} // namespace net_op
} // namespace pluginxx

#endif /* PLUGINXX_NET_OPS_H */
