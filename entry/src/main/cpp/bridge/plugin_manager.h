#pragma once
#include "graphics/egl_renderer.h"
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <queue>

// surfaceId 驱动的 XComponent 管理
//
// 每个 XComponent 通过自定义 Controller 回调拿到的 surfaceId 是唯一的,
// 不再共享 libraryname → exports 对象, 从根本上消除多窗口 XComponent 冲突。
//
// 架构:
//   WineWindow.ets (XComponentController.onSurfaceCreated)
//     → NAPI createRenderer(toplevelId, surfaceId)
//     → PluginManager::CreateRenderer → OH_NativeWindow_CreateNativeWindowFromSurfaceId
//     → EglRenderer::Init(nativeWindow, 1, 1) → 共享 EGLDisplay + 独立 EGLContext
//   WineWindow.ets (XComponentController.onSurfaceChanged)
//     → NAPI resizeRenderer(toplevelId, w, h)
//     → PluginManager::ResizeRenderer → EglRenderer::SetSize
//   WineWindow.ets (XComponentController.onSurfaceDestroyed)
//     → NAPI destroyRenderer(toplevelId) → DestroyToplevel
//
// 键盘事件仅通过 Stack.onKeyEvent → NAPI sendKeyEvent → InputManager 路径,
// 不再使用 OH_NativeXComponent_RegisterKeyEventCallback。
class PluginManager {
public:
    static PluginManager* GetInstance();

    // surfaceId 驱动的渲染器生命周期
    void CreateRenderer(uint32_t toplevelId, int64_t surfaceId);
    void ResizeRenderer(uint32_t toplevelId, int w, int h);
    // 保留 NativeWindow/EGLContext 的安全重绑定路径。仅请求 Wayland 重新提交
    // 当前帧；XComponent 的真实 surface 销毁/创建仍必须走 Create/Destroy 生命周期。
    void RefreshRenderer(uint32_t toplevelId);
    void DestroyToplevel(uint32_t toplevelId);

    // pending toplevelId 队列: Ability 在 loadContent 前入队
    // WineWindow.aboutToAppear 同步出队 (FIFO, 无竞态)
    void SetPendingToplevel(uint32_t id) { pendingToplevelQueue_.push(id); }
    uint32_t DequeuePendingToplevel();

    // 辅助: toplevelId -> EglRenderer 查找 (InputManager 坐标转换使用)
    EglRenderer* GetRendererForToplevel(uint32_t tid);
    // Desktop 合成模式: 取当前登记的唯一 renderer（输入坐标映射兜底）。
    // RootCompositing 下所有 renderer 都渲染桌面根，letterbox 与登记 id
    // 无关；前台窗口"提升"导致根 id 上查不到 renderer 时用它仍能正确映射。
    EglRenderer* GetAnyRenderer();
    /** 窗口可见性变化时暂停/恢复对应 renderer (后台时避免 vsync/swap 阻塞)。 */
    void SetRendererPaused(uint32_t toplevelId, bool paused);
    // Desktop 模式: root 切换时更新渲染器的 toplevel 映射
    void MoveRendererToToplevel(uint32_t oldId, uint32_t newId);
    size_t GetRendererCount() const { return toplevelRenderers_.size(); }

private:
    PluginManager() = default;

    // 每个 toplevel 一个独立 EGLContext 渲染器
    std::unordered_map<uint32_t, std::unique_ptr<EglRenderer>> toplevelRenderers_;

    // pending queue: Ability 入队, WineWindow.aboutToAppear 出队 (FIFO 无竞态)
    std::queue<uint32_t> pendingToplevelQueue_;
};
