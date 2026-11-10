# 变更记录

## 0.3.0

- **能力行带作用域** (`<应用名>.capabilities` v5 → **v6**)
  - `CapabilityProvider` 新增 `scopeId`: 登记时从 `PluginxxScopeRef` 拷贝 (空 = 进程级,
    对所有作用域生效);
  - 覆盖键改成 `(provider, scopeId, ownerTag, phase)`; `impls: one` 的 impl 行**每个作用域
    各一份** (同一插件被多个作用域各挂载一次时不再互相覆盖);
  - 行查询 `rowsOf(id, phase, scopeId)` (只取空作用域的行 + 同作用域的行),
    `removeScope(scopeId)` 一次清掉一个作用域的全部行;
  - 表尾追加 `unregister_capability_ex` (按作用域注销一条实现行); 基线不变。
- **宿主 (应用) 发起能力调用**: `OpCore::createDetached()` 建一个没有实例租约的操作;
  `PluginHostCore::invokeCapabilityAsync` 的 `caller` 允许为空 (主程序自己发起)。
  无主操作的句柄由 `PluginRuntime::detachedHandles` 保活, 调用方手里的裸指针在完成后
  仍可安全地传给取消入口 (已完成时是空操作)。
- **广播流结束** (`<应用名>.broadcasts` v2 → **v3**)
  - `BroadcastRegistry::end(id, reasonJson, writer)`: 属主或应用可结束一条流;
  - 结束后: 发布被拒 (`PLUGINXX_RESULT_ERR_ENDED`)、`read` 不给值、全部订阅摧除并回调
    `onEnd` 恰好一次; 再次 `declare` 同一个 id = 开一条新流 (清结束标记与缓存);
  - 表尾追加 `end_broadcast` / `subscribe_broadcast_ex`; 基线不变。
- **功能埋点原语** (`PluginHostCore`): `PluginPointDecl` + `declarePoint` / `emitPointAsync` /
  `endPoint` —— 一个埋点 = 一个能力 id (声明为 `impls: many` + `merge: first`, 只开 `impl`)
  + 一个同名广播频道。`emitPointAsync` 先发广播, 再按作用域调实现链; 没有实现行时
  直接按 `OK` + `null` 完成 ("没人实现 = 没意见")。
- `capability_chain.h` 的 `appendStage` 多一个可选 `scopeId` (空 = 不过滤)。
- `api_abi_check.c`: 新增两张表"基线止于第一个追加成员"的静态断言。

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
