// Native Linux Vulkan X11 -> Wayland window ownership and input routing.
// Only protocol/input crosses XCB. Rendered pixels go through Venus/NativeImage.
#ifndef STEAMOHOS_NATIVE_X11_WINDOW_H
#define STEAMOHOS_NATIVE_X11_WINDOW_H
#include "xdg-shell-client-protocol.h"
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <sys/uio.h>
#include <poll.h>
#include <thread>
#include <string>
#include <array>
#include <cmath>
namespace steamohos {
class NativeX11Window {
    xcb_connection_t* x=nullptr;
    xcb_window_t xid=0;
    wl_registry* registry=nullptr;wl_compositor* compositor=nullptr;xdg_wm_base* wm=nullptr;
    xdg_surface* shell=nullptr;xdg_toplevel* top=nullptr;
    wl_seat* seat=nullptr;wl_pointer* pointer=nullptr;wl_keyboard* keyboard=nullptr;
    std::thread events;std::atomic<bool> stopping{false},lost{false};
    bool configured=false;uint32_t width=0,height=0;
    double wheel[2]={0,0};int discrete[2]={0,0};std::array<bool,256> keys{};
    xcb_extension_t xtest{"XTEST",0};
    struct FakeInput {
        uint8_t major,minor;uint16_t length;uint8_t type,detail;uint16_t pad0;
        uint32_t time,root,pad1,pad2;int16_t x,y;uint32_t pad3;uint16_t pad4;uint8_t pad5,device;
    };
    static_assert(sizeof(FakeInput)==36,"XTEST 2.2 wire ABI");
    void Input(uint8_t type,uint8_t detail) {
        FakeInput request{};request.type=type;request.detail=detail;
        const xcb_protocol_request_t protocol{2,&xtest,2,1};
        iovec parts[4]{};parts[2]={&request,sizeof(request)};parts[3]={nullptr,0};
        xcb_send_request(x,0,parts+2,&protocol);xcb_flush(x);
    }
    void Focus() {
        xcb_set_input_focus(x,XCB_INPUT_FOCUS_POINTER_ROOT,xid,XCB_CURRENT_TIME);xcb_flush(x);
    }
    void ReleaseKeys() {
        for(unsigned i=0;i<keys.size();++i)if(keys[i]){Input(XCB_KEY_RELEASE,i);keys[i]=false;}
    }
    void CloseRequest() {
        xcb_generic_error_t* error=nullptr;
        auto* protocol=xcb_intern_atom_reply(x,xcb_intern_atom(x,0,12,"WM_PROTOCOLS"),&error);free(error);error=nullptr;
        auto* close=xcb_intern_atom_reply(x,xcb_intern_atom(x,0,16,"WM_DELETE_WINDOW"),&error);free(error);
        if(protocol&&close) {
            xcb_client_message_event_t message{};message.response_type=XCB_CLIENT_MESSAGE;message.window=xid;
            message.type=protocol->atom;message.format=32;message.data.data32[0]=close->atom;message.data.data32[1]=XCB_CURRENT_TIME;
            xcb_send_event(x,0,xid,XCB_EVENT_MASK_NO_EVENT,reinterpret_cast<const char*>(&message));xcb_flush(x);
        }
        free(protocol);free(close);
    }
    void Wheel() {
        for(unsigned axis=0;axis<2;++axis) {
            const double amount=discrete[axis]?discrete[axis]:wheel[axis]/10.0;
            const int steps=std::clamp(static_cast<int>(amount),-32,32);
            const uint8_t button=axis?(steps>0?7:6):(steps>0?5:4);
            for(int i=0;i<std::abs(steps);++i){Input(XCB_BUTTON_PRESS,button);Input(XCB_BUTTON_RELEASE,button);}
            wheel[axis]=discrete[axis]?0:wheel[axis]-steps*10.0;discrete[axis]=0;
        }
    }
    static void Globals(void* data,wl_registry* r,uint32_t id,const char* name,uint32_t version) {
        auto& self=*static_cast<NativeX11Window*>(data);
        if(!strcmp(name,"wl_compositor"))self.compositor=static_cast<wl_compositor*>(wl_registry_bind(r,id,&wl_compositor_interface,std::min(version,4u)));
        else if(!strcmp(name,"xdg_wm_base")) {
            self.wm=static_cast<xdg_wm_base*>(wl_registry_bind(r,id,&xdg_wm_base_interface,1));
            static const xdg_wm_base_listener listener{[](void*,xdg_wm_base* wm,uint32_t serial){xdg_wm_base_pong(wm,serial);}};
            xdg_wm_base_add_listener(self.wm,&listener,&self);
        } else if(!strcmp(name,"wl_seat")) {
            self.seat=static_cast<wl_seat*>(wl_registry_bind(r,id,&wl_seat_interface,std::min(version,5u)));
            static const wl_seat_listener listener{Capabilities,[](void*,wl_seat*,const char*){}};
            wl_seat_add_listener(self.seat,&listener,&self);
        }
    }
    static void Capabilities(void* data,wl_seat* seat,uint32_t capabilities) {
        auto& self=*static_cast<NativeX11Window*>(data);
        if((capabilities&WL_SEAT_CAPABILITY_POINTER)&&!self.pointer) {
            self.pointer=wl_seat_get_pointer(seat);
            static const wl_pointer_listener listener=[] {
                wl_pointer_listener p{};
                p.enter=[](void* d,wl_pointer*,uint32_t,wl_surface*,wl_fixed_t,wl_fixed_t){static_cast<NativeX11Window*>(d)->Focus();};
                p.leave=[](void*,wl_pointer*,uint32_t,wl_surface*){};
                p.motion=[](void* d,wl_pointer*,uint32_t,wl_fixed_t x,wl_fixed_t y){
                    auto& s=*static_cast<NativeX11Window*>(d);
                    xcb_warp_pointer(s.x,XCB_NONE,s.xid,0,0,0,0,
                        std::clamp<int>(wl_fixed_to_int(x),0,32767),std::clamp<int>(wl_fixed_to_int(y),0,32767));xcb_flush(s.x);
                };
                p.button=[](void* d,wl_pointer*,uint32_t,uint32_t,uint32_t button,uint32_t state){
                    uint8_t detail=button==0x110?1:button==0x111?3:button==0x112?2:button==0x113?8:button==0x114?9:0;
                    if(detail)static_cast<NativeX11Window*>(d)->Input(state?XCB_BUTTON_PRESS:XCB_BUTTON_RELEASE,detail);
                };
                p.axis=[](void* d,wl_pointer*,uint32_t,uint32_t axis,wl_fixed_t amount){if(axis<2)static_cast<NativeX11Window*>(d)->wheel[axis]+=wl_fixed_to_double(amount);};
                p.frame=[](void* d,wl_pointer*){static_cast<NativeX11Window*>(d)->Wheel();};
                p.axis_source=[](void*,wl_pointer*,uint32_t){};
                p.axis_stop=[](void*,wl_pointer*,uint32_t,uint32_t){};
                p.axis_discrete=[](void* d,wl_pointer*,uint32_t axis,int32_t amount){if(axis<2)static_cast<NativeX11Window*>(d)->discrete[axis]+=amount;};
                return p;
            }();
            wl_pointer_add_listener(self.pointer,&listener,&self);
        }
        if(!(capabilities&WL_SEAT_CAPABILITY_POINTER)&&self.pointer){wl_pointer_destroy(self.pointer);self.pointer=nullptr;}
        if((capabilities&WL_SEAT_CAPABILITY_KEYBOARD)&&!self.keyboard) {
            self.keyboard=wl_seat_get_keyboard(seat);
            static const wl_keyboard_listener listener=[] {
                wl_keyboard_listener k{};
                k.keymap=[](void*,wl_keyboard*,uint32_t,int32_t fd,uint32_t){close(fd);};
                k.enter=[](void* d,wl_keyboard*,uint32_t,wl_surface*,wl_array*){static_cast<NativeX11Window*>(d)->Focus();};
                k.leave=[](void* d,wl_keyboard*,uint32_t,wl_surface*){static_cast<NativeX11Window*>(d)->ReleaseKeys();};
                k.key=[](void* d,wl_keyboard*,uint32_t,uint32_t,uint32_t key,uint32_t state){
                    auto& s=*static_cast<NativeX11Window*>(d);if(key>247)return;key+=8;
                    s.keys[key]=state!=0;s.Input(state?XCB_KEY_PRESS:XCB_KEY_RELEASE,key);
                };
                k.modifiers=[](void*,wl_keyboard*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){};
                k.repeat_info=[](void*,wl_keyboard*,int32_t,int32_t){};return k;
            }();
            wl_keyboard_add_listener(self.keyboard,&listener,&self);
        }
        if(!(capabilities&WL_SEAT_CAPABILITY_KEYBOARD)&&self.keyboard){self.ReleaseKeys();wl_keyboard_destroy(self.keyboard);self.keyboard=nullptr;}
    }
    bool Dispatch(int timeout) {
        while(wl_display_prepare_read(display)!=0)if(wl_display_dispatch_pending(display)<0)return false;
        int flushed=wl_display_flush(display);
        if(flushed<0&&errno!=EAGAIN){wl_display_cancel_read(display);return false;}
        pollfd fd{wl_display_get_fd(display),POLLIN,0};int ready;
        do{ready=poll(&fd,1,timeout);}while(ready<0&&errno==EINTR&&!stopping);
        if(ready>0&&(fd.revents&POLLIN)) {
            if(wl_display_read_events(display)<0)return false;
        } else wl_display_cancel_read(display);
        return ready>=0&&!(fd.revents&(POLLERR|POLLHUP|POLLNVAL))&&wl_display_dispatch_pending(display)>=0;
    }
    bool Roundtrip() {
        bool done=false;wl_callback* callback=wl_display_sync(display);if(!callback)return false;
        static const wl_callback_listener listener{[](void* p,wl_callback*,uint32_t){*static_cast<bool*>(p)=true;}};
        wl_callback_add_listener(callback,&listener,&done);
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!done&&std::chrono::steady_clock::now()<deadline)if(!Dispatch(50))break;
        wl_callback_destroy(callback);return done;
    }
public:
    wl_display* display=nullptr;wl_surface* surface=nullptr;
    bool Lost() const {return lost;}
    bool Create(uint32_t window) {
        if(!window)return false;xid=window;
        x=xcb_connect(nullptr,nullptr);if(!x||xcb_connection_has_error(x))return false;
        xcb_generic_error_t* error=nullptr;
        auto* info=xcb_get_geometry_reply(x,xcb_get_geometry(x,xid),&error);free(error);
        if(!info)return false;width=info->width;height=info->height;free(info);
        const auto* test=xcb_get_extension_data(x,&xtest);if(!test||!test->present)return false;
        display=wl_display_connect(nullptr);if(!display)return false;
        registry=wl_display_get_registry(display);
        static const wl_registry_listener listener{Globals,[](void*,wl_registry*,uint32_t){}};
        wl_registry_add_listener(registry,&listener,this);
        if(!Roundtrip()||!compositor||!wm)return false;
        surface=wl_compositor_create_surface(compositor);if(!surface)return false;
        shell=xdg_wm_base_get_xdg_surface(wm,surface);if(!shell)return false;
        static const xdg_surface_listener configuredListener{[](void* p,xdg_surface* shell,uint32_t serial){
            xdg_surface_ack_configure(shell,serial);static_cast<NativeX11Window*>(p)->configured=true;
        }};
        xdg_surface_add_listener(shell,&configuredListener,this);
        top=xdg_surface_get_toplevel(shell);if(!top)return false;
        static const xdg_toplevel_listener topListener=[] {
            xdg_toplevel_listener t{};
            t.configure=[](void* p,xdg_toplevel*,int32_t w,int32_t h,wl_array*) {
                auto& s=*static_cast<NativeX11Window*>(p);
                if(w>0&&h>0&&w<=4096&&h<=4096) {
                    s.width=w;s.height=h;const uint32_t values[]={s.width,s.height};
                    xcb_configure_window(s.x,s.xid,XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,values);xcb_flush(s.x);
                }
            };
            t.close=[](void* p,xdg_toplevel*){static_cast<NativeX11Window*>(p)->CloseRequest();};return t;
        }();
        xdg_toplevel_add_listener(top,&topListener,this);
        xdg_toplevel_set_app_id(top,"hmosdeck.native-linux-game");
        xdg_toplevel_set_title(top,"Linux game");
        auto* title=xcb_get_property_reply(x,xcb_get_property(x,0,xid,XCB_ATOM_WM_NAME,XCB_GET_PROPERTY_TYPE_ANY,0,128),nullptr);
        if(title&&title->format==8&&xcb_get_property_value_length(title)>0) {
            std::string text(static_cast<char*>(xcb_get_property_value(title)),xcb_get_property_value_length(title));
            xdg_toplevel_set_title(top,text.c_str());
        }
        free(title);xdg_toplevel_set_fullscreen(top,nullptr);wl_surface_commit(surface);
        if(!Roundtrip()||!configured)return false;
        events=std::thread([this]{while(!stopping)if(!Dispatch(50)){lost=true;break;}});
        fprintf(stderr,"STEAMOHOS_NATIVE_VULKAN x11_window=%u wayland_surface=%u input=xtest pixel_readbacks=0\n",xid,wl_proxy_get_id(reinterpret_cast<wl_proxy*>(surface)));
        return true;
    }
    ~NativeX11Window() {
        stopping=true;if(events.joinable())events.join();
        if(x)ReleaseKeys();
        if(keyboard)wl_keyboard_destroy(keyboard);if(pointer)wl_pointer_destroy(pointer);if(seat)wl_seat_destroy(seat);
        if(top)xdg_toplevel_destroy(top);if(shell)xdg_surface_destroy(shell);if(surface)wl_surface_destroy(surface);
        if(wm)xdg_wm_base_destroy(wm);if(compositor)wl_compositor_destroy(compositor);if(registry)wl_registry_destroy(registry);
        if(display){wl_display_flush(display);wl_display_disconnect(display);}if(x)xcb_disconnect(x);
    }
};
}
#endif
