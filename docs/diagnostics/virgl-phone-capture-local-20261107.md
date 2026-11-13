# 本地 VirGL 手机诊断代码归档

`virgl-phone-capture-local-20261107.diff` 保存了 2026-11-10 核对时开发工作树中尚未提交的 VirGL 手机 GPU 捕获、检查点和着色器重放代码。它是诊断快照，不是已验证的兼容性修复；正常构建和发布不会应用该文件。原始本地工作树未被修改。

- 基线：应用仓库提交 `c02c2f0b82df0c7b14d32c165fdc303da452cd04`，`thirdparty/virglrenderer` gitlink 为 `bb33e52aa03199926f662e50ff22bd00d19d7e0a`。
- 基线状态：先对上述 gitlink 执行仓库版本的 `scripts/apply_virglrenderer_ohos_patches.sh`。快照记录的是该受控补丁集与原本地工作树之间的差异，包含新增的 `src/venus/vkr_phone_replay.h`。
- SHA-256：`b62f87b97a20515d0d24a9ace6798a235f70ece8fa69b93b83c737f09a65d574`。
- 验证：在独立克隆、上述固定 gitlink 和已应用正式补丁的状态下，`git apply --check` 和实际应用均成功；应用后 VirGL 的已跟踪文件差异及新增诊断头文件与原本地工作树逐字一致。随后已撤销该诊断补丁。

需要重新调查时，在隔离检出中先检查差异，再从 VirGL 子模块根目录执行：

```sh
git apply --check ../../docs/diagnostics/virgl-phone-capture-local-20261107.diff
git apply ../../docs/diagnostics/virgl-phone-capture-local-20261107.diff
```

该代码受 `WINEHUA_VKR_TRACE_CAPTURE` 控制，包含 Vulkan 检查点、日志捕获和特定着色器的本机重放，可能改变运行时行为。它尚未作为正式构建进行审查和设备回归，不应直接纳入发布；使用前需重新评估设备、性能及兼容性。
