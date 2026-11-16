# 2.0.3 主线整合记录（2026-11-16）

本次将所有有效功能收拢到 main，作为下一轮大合并的基线。产品版本保持 2.0.3 / 2000030。未重新构建、签名、安装或上传应用，不将合并视为新增真机验收。

## 收拢范围

- `feature/fex-proton-upstream-sync` 的主线增量：完整 Wine/FEX 上游迁移、Gecko、手机初始化与 Mesa EGL/Gallium 配对、图形性能测量和兼容性修复、VirGL / Zink / DXVK 按游戏切换、包月与买断购买修复、2.0.3 发布代码及多手柄。
- `feature/main_proton` 的三个独有提交（4bad2d4d、ea9f0199、b75c6753）保留完整合并历史。逐个冲突核对后，合并结果与当前 feature 源码树一致：保留新的 DXVK Modern pin、受管理 overlay 前置、upstream Wine stamp 路径和显式 D3D9 路由，没有回退旧实现。
- 旧工作树中未提交的 subsurface 生命周期保护、输入锁域和线程退出元数据校验，当前已有相同或更新实现；Steam 启动已统一到受管理的 steam.exe 参数策略，保留当前冷/热启动及缓存卡片回归，不复原旧批处理启动策略。
- 两份未登记、但保留研究价值的独立补丁存入 `docs/upstream-sync/reference/`，标明原始基线、校验值和未启用状态；不是正式构建输入。
- Zink 版本研究归档到 `docs/graphics/zink-opengl-version-research-20261116.*`。只保留真实 GL3.1 对齐目标，更高版本暂停，未通过版本 override 冒报支持。

## 补齐整合校验

更新 `graphics-stack.lock.yaml`：登记 0083 多手柄 overlay、实际 Winebus/协议源码和构建入口校验值，并登记此前遗漏的 Mesa 0014 诊断补丁库存。0014 仍未加入产品 apply recipe，不启用额外诊断。修复前校验因登记缺失失败，补齐后完整源码合同校验通过。

## 第三方与缓存边界

对 Mesa 23、VirGL 20、DXVK Legacy 19、Wine 80、wlroots 9、Xserver 2 个正常构建 overlay 进行独立 index 重放，全部成功，不改实际第三方 index 或源码。Box64 的 pinned overlay 身份核验通过；GLib 格式/intl 与 GStreamer CUDA 修改均已由现有构建脚本保存。

VirGL 重放后的额外变化是临时 GPU flight/checkpoint/phone shader replay，未加入正式 overlay；Wine 的其他变化分别由 upstream 0078/0079、生成入口或更完整的 0082 保存。旧 RWX SMC 试验由当前完整 Windows VM/SMC 合同取代，不复制过时实现。旧 vkd3d 嵌套依赖的 diff 去除行尾差异后为空，不引入换行噪声。

原有十个 dirty 子模块及全部已登记文件逐 SHA-256 核对未变；它们仍会显示 dirty，不能用 reset/clean 伪造干净状态。没有把缓存目录、源码解包树、`.orig/.rej/.wraplock`、包、日志或审计 Git objects 加入提交。Git 文件清单检查未发现上述中间成果。

## 验证边界

15 组主机回归全部通过：四手柄适配器/Hub/socket、Winebus、原 bridge、subsurface 生命周期、生产 GPU 场景输入、线程退出包校验、运行库校验、Mesa 构建身份、发布边界、FEX VM 合同、游戏后端设置、Steam 服务、会员服务、购买 UI 和会员后端。图形源码合同校验通过。精确命令与结果见同目录 JSON。

完整 `scripts/ci_preflight.py` 预检 273 项通过。发布验收样本已排除 smoke 测试库，并显式验证混入 smoke 时必须拒绝；DXVK overlay 回放样本从 `+++ b/` 收集全部受影响文件，同时支持标准 Git diff 与普通 unified diff。没有放宽产品包校验或修改实际运行行为。

本次没有实体多手柄验收；继续保留 `hardwareValidated=false`，等待用户连接多只手柄验证。未新增游戏性能或 GL3.1 能力结论。已存在的多手柄调试 HAP 可继续测试；已有正式签名 APP 不含随后新增的多手柄修复，不能称为本次主线的重新打包。
