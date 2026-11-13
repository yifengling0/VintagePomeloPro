# Proton native 声明契约修正

与会员功能独立的窄修；不修改 C++、Wine、渲染、输入运行逻辑或三平台配置。

## 依据

- `bridge/napi_init.cpp:945,1345–1594`：图形/窗口/IME/日志/session API 的真实参数与返回。
- `input/controller/controller_napi.cpp:46–245`：controller enabled/button/stick/hat/source/mode/state text。
- `input/game_controller_bridge.cpp:67–110,332–380`：四种 JS 回调实际形状及初始化返回值。
- `bridge/performance_monitor_napi.cpp:42–75`：双 boolean 参数，返回 `Promise<string>`。
- `wine/wine_exe.cpp:348–387,423–565`：RunWineProgram 实际读取字段、RunWineExe 十参数和 `{pid,sessionId,reused}`。

原声明缺 33 个已有真实导出，runWineExe 错写五参数/void。补齐精确声明，无 any、unknown、伪实现或强制断言掩盖编译。

## 不支持的语义保持诚实

当前 proton 的 getWineSession 固定返回 null；activateWineSession/stopWineSession 固定 false。声明不代表这些功能已恢复。

RunWineProgram 不读取 prefixMode、wineLang、presentToSurface、automationMode，也不读取独立 dxvkBackend。它从 d3dBackend 派生 DXVK 和默认 presentBackend。SmokeRunner 改为只传真实字段，automation/offscreen 继续走已有 exe argv 协议。clean-prefix 请求明确返回 UNSUPPORTED，不启动测试、不切换/恢复产品会话。不支持的 D3D backend 或独立 DXVK 组合同样报告 UNSUPPORTED，避免 native 静默退回其他后端形成虚假测试结果。

## 验证

`scripts/run_native_contract_unit_tests.cjs <官方SDK typescript.js路径>` 对实际 SmokeRunner 转译执行，检查 33 导出、十参结构、clean/backend 闭锁、无产品恢复调用，以及正常 present/offscreen argv。此测试已加入模型测试入口。

隔离全 entry ArkTS 第 6、7 轮均编译成功，64 条原声明诊断消除；第7轮已核最终声明/SmokeRunner 源码 hash。包含后续云档扩展的第8轮也通过。此结果不是 native 链接/HAP/真机或 smoke 图形测试通过；没有重编 Wine。
