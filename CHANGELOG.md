# 变更记录

## 0.2.0

- **应用名**: 新增 CMake 缓存变量 `PLUGINXX_APP_NAME` (默认 `pluginxx`), 以 PUBLIC 编译定义下发给
  所有使用方; 新增 `pluginxx/api/app.h` (`PLUGINXX_APP_NAME` / `PLUGINXX_APP_ID(name)` /
  `pluginxx::appIdPrefix()`)。主程序必须覆盖它, 否则通用表 IID 拼出来不是它以 `.` 开头的命名空间。
- **接口表 IID**: 10 张通用表的 IID 从字面量 `pluginxx.<表名>` 改成 `PLUGINXX_APP_ID("<表名>")`,
  即 `<应用名>.<表名>`; 表名部分仍然冻结, 应用名随构建配置变。不兼容: 宿主与插件必须用同一个
  应用名编译 (SDK 的 INTERFACE 定义统一下发), 否则插件按旧前缀查不到表。
- **官方标识前缀可配**: `PluginHostCore` 新增 `setOfficialPrefix()` / `officialPrefix()` /
  `isOfficialId()` 成员; 写死的 `detail::isOfficialId` 改成纯函数
  `detail::isOfficialIdWithPrefix(id, prefix)` (供测试与工具使用)。库内不再出现任何具体项目名。
- **诊断**: `queryGenericPluginIface` 未命中且 IID 以 `.<已知通用表名>` 结尾时记一条
  "应用名不一致" 的日志 (只提示, 不改行为)。
- 注释修正: `tables.h` 的 `capabilities` 版本写成 v4 (实际 v5); `README.md` 与 `tables_impl.h`
  的通用表列表写成 `events` (实际是 `broadcasts`)。

## 0.1.0

- 首个版本: 纯 C ABI 契约、通用接口表、插件运行时与插件 SDK 核心。
