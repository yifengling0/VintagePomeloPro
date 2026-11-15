# PC 系统桌面 OpenGL：保留项目

状态：暂缓。2026-11-15用户决定，未来PC单开分支适配，当前优先通用图形路线。此文档不代表已经建立分支或开始部署。

参考现有系统GL调研：`mini-backend-adaptation-20261115.zh-CN.md`。用户提供7.0.0.111、Edge/铠大师GL4.1线索；官方文档有桌面GL公开接口。具体设备与普通VPP进程可用性需实际能力查询。

恢复条件与顺序：

1. 有可用PC目标后，从当时稳定通用分支建立独立适配分支。
2. 在普通应用及renderer child用公开API查询和创建真实desktop GL context，验证renderer、扩展、基础绘制；确认NEED_OPENGL启动配置与失败时GLES回退。
3. 成套处理renderer/epoxy dispatch、共享context、GLSL、NativeBuffer导入/呈现与双向fence，保留通用GLES路线。
4. 按真实capset暴露guest能力；完成PE32/PE64正确性与同设置性能比较后，再考虑合入。

当前不实施：提高Mini系统要求、修改通用默认后端、伪造GL版本、部署Edge/PC试验包。

收益需测量：系统GL可能减少GLES状态仿真和shadow copy，VirGL编码/传输成本仍需独立优化。GL版本提升本身不是帧率收益证据。
