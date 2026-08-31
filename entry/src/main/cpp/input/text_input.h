#pragma once

#include <wayland-server-core.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/*
 * zwp_text_input_manager_v3 的 compositor 服务端。
 *
 * 设计原则: text-input 焦点必须与键盘注入完全同源。InputManager 在每次
 * 点击/按键时维护权威的 keyboardFocusedSurface_, 本类只做它的镜像:
 *   - InputManager::InjectKeyboardEnter -> OnKeyboardEnter (发 enter)
 *   - InputManager::InjectKeyboardLeave -> OnKeyboardLeave (发 leave)
 * 这样 Wine 收到的 commit/preedit 目标, 就是 wl_keyboard.key 实际注入的
 * 同一个 surface/client, 不再有第二套焦点状态。
 *
 * 线程模型:
 *   - 协议请求/焦点钩子: Wayland 线程
 *   - IsEnabled 等查询: NAPI 线程 (只读, mutex 保护)
 */
class TextInputManager {
public:
    static TextInputManager* GetInstance();

    // 激活回调 (对齐上游 54d188d): active=1 且带光标矩形 → ArkTS 弹系统软键盘;
    // active=0 (失焦/leave) → 收起。
    using ActivateCb = std::function<void(bool active, int x, int y, int w, int h)>;
    void SetActivateCallback(ActivateCb cb);

    void Register(wl_display* display);

    // Wayland 线程: 键盘焦点跟随
    void OnKeyboardEnter(uint32_t toplevelId, wl_resource* surface);
    void OnKeyboardLeave();
    void OnSurfaceDestroyed(wl_resource* surface);

    bool IsEnabled() const;

    // -- NAPI 线程入口 (preedit/commit 入队, Wayland 线程统一发送) --
    bool SendPreedit(const char* utf8, int32_t cursorBegin, int32_t cursorEnd);
    bool SendCommit(const char* utf8);
    bool SendDeleteSurrounding(uint32_t before, uint32_t after);
    void SetArmed(bool armed);

    // -- 协议请求 (client -> server, Wayland 线程) --
    static void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void manager_destroy(wl_client*, wl_resource* r);
    static void manager_get_text_input(wl_client*, wl_resource* manager, uint32_t id,
                                       wl_resource* seat);
    static void ti_destroy(wl_client*, wl_resource* r);
    static void ti_enable(wl_client*, wl_resource* r);
    static void ti_disable(wl_client*, wl_resource* r);
    static void ti_set_surrounding_text(wl_client*, wl_resource*, const char*, int32_t, int32_t);
    static void ti_set_text_change_cause(wl_client*, wl_resource*, uint32_t);
    static void ti_set_content_type(wl_client*, wl_resource*, uint32_t, uint32_t);
    static void ti_set_cursor_rectangle(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t);
    static void ti_commit(wl_client*, wl_resource* r);

private:
    struct Entry {
        wl_resource* res = nullptr;
        wl_resource* enteredSurface = nullptr;
        bool enabled = false;
        uint32_t commitCount = 0;
        // 光标矩形 (ti_set_cursor_rectangle 存入): 激活判定 enabled+非零矩形。
        int32_t cursorX = 0, cursorY = 0, cursorW = 0, cursorH = 0;
        bool activated = false;
    };

    enum class OpType { Preedit, Commit, DeleteSurrounding, Done, SetArmed };

    struct Op {
        OpType type = OpType::Preedit;
        wl_resource* res = nullptr;
        std::string text;
        int32_t begin = 0;
        int32_t end = 0;
        bool armed = false;
    };

    TextInputManager() = default;
    static void resource_destroyed(wl_resource* r);
    void EnqueueOp(Op op);
    static int OnPipeReadable(int fd, uint32_t mask, void* data);
    void FlushOps();
    Entry* EnabledEntryLocked();
    void LeaveAllEnteredLocked(std::vector<std::pair<wl_resource*, wl_resource*>>& actions);
    // 激活判定/回调 (Wayland 线程): enabled + 非零光标矩形 → 回调 ArkTS。
    void MaybeActivateLocked();
    void NotifyActivated(bool active);

    wl_global* global_ = nullptr;
    wl_display* display_ = nullptr;
    // 由 NAPI wineTextInputSetArmed 门控: 宿主键盘打开才 armed。
    bool armed_ = false;
    uint32_t focusedToplevel_ = 0;
    wl_resource* focusedSurface_ = nullptr;
    ActivateCb activateCb_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;

    std::mutex opMutex_;
    std::vector<Op> opQueue_;
    int pipeReadFd_ = -1;
    int pipeWriteFd_ = -1;
    struct wl_event_source* pipeSource_ = nullptr;
};
