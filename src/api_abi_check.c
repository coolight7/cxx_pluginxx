/// pluginxx C ABI 头的纯 C 编译校验
///
/// 目的: `pluginxx/api/abi.h` 与 `pluginxx/api/tables.h` 必须能被 **C 编译器**
/// 直接包含 (跨边界契约不依赖 C++ 特性), 且结构体满足 8 字节对齐约定。
/// 本文件以 C 语言编译 (c17) 包含这两个头, 并做编译期断言; 一旦有人在 C ABI
/// 头里引入 C++ 语法或破坏对齐/定长类型约定, 构建即失败。

#include "pluginxx/api/abi.h"
#include "pluginxx/api/tables.h"

/* 8 字节对齐约定: 跨边界结构体不得出现更大的对齐要求 (避免平台差异) */
_Static_assert(
    _Alignof(PluginxxStringView) <= 8,
    "PluginxxStringView 对齐超过 8 字节"
);
_Static_assert(_Alignof(PluginxxHost) <= 8, "PluginxxHost 对齐超过 8 字节");
_Static_assert(_Alignof(PluginxxOperatorNotify) <= 8, "PluginxxOperatorNotify 对齐超过 8 字节");

/* 定长基础类型约定: 字符串视图的长度字段必须是 uint64_t */
_Static_assert(
    sizeof(((PluginxxStringView*)0)->size) == 8,
    "PluginxxStringView::size 必须是 8 字节定长类型"
);

/* 入口符号名由宿主提供 (内核不定义宿主专名, 见 pluginxx/api/entry.h):
   这里不再校验具体符号名, 改为在 C++ 侧校验宿主接缝 (entrySymbols) 的语义 */

const char* pluginxx_api_abi_probe(void);

/// 返回宿主日志表的 IID (证明通用表声明在本 TU 可见且可用于 C)
const char* pluginxx_api_abi_probe(void) {
    return PLUGINXX_IFACE_LOG;
}
