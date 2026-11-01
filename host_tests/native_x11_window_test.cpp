// Real XTEST input, Wayland configure/close and protocol lifetime; no GPU.
#include <wayland-server.h>
#include "xdg-shell-server-protocol.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include "../entry/src/main/cpp/linux_steam/gnu_fixture/native_x11_window.h"
namespace {
struct Server {
 wl_display* display=nullptr;wl_resource* surface=nullptr;wl_resource* shell=nullptr;
 wl_resource* top=nullptr;wl_resource* pointer=nullptr;wl_resource* keyboard=nullptr;
 uint32_t serial=10;std::atomic<bool> inputReady{false};
 static Server& Get(wl_resource* r){return *static_cast<Server*>(wl_resource_get_user_data(r));}
 static void Destroy(wl_client*,wl_resource* r){wl_resource_destroy(r);}
 static void Surface(wl_client* c,wl_resource* parent,uint32_t id) {
  auto& s=Get(parent);auto* r=wl_resource_create(c,&wl_surface_interface,4,id);s.surface=r;
  static const struct wl_surface_interface api=[] {
   struct wl_surface_interface x{};x.destroy=Destroy;
   x.attach=[](wl_client*,wl_resource*,wl_resource*,int32_t,int32_t){};
   x.damage=[](wl_client*,wl_resource*,int32_t,int32_t,int32_t,int32_t){};
   x.frame=[](wl_client* c,wl_resource*,uint32_t id){auto* r=wl_resource_create(c,&wl_callback_interface,1,id);wl_callback_send_done(r,0);wl_resource_destroy(r);};
   x.set_opaque_region=[](wl_client*,wl_resource*,wl_resource*){};x.set_input_region=x.set_opaque_region;
   x.commit=[](wl_client*,wl_resource* r){auto& s=Get(r);if(s.shell&&s.top){wl_array states;wl_array_init(&states);xdg_toplevel_send_configure(s.top,800,600,&states);xdg_surface_send_configure(s.shell,++s.serial);wl_array_release(&states);}};
   x.set_buffer_transform=[](wl_client*,wl_resource*,int32_t){};x.set_buffer_scale=x.set_buffer_transform;
   x.damage_buffer=x.damage;return x;
  }();wl_resource_set_implementation(r,&api,&s,nullptr);
 }
 static void Compositor(wl_client* c,void* data,uint32_t version,uint32_t id) {
  auto* r=wl_resource_create(c,&wl_compositor_interface,std::min(version,4u),id);
  static const struct wl_compositor_interface api{Surface,[](wl_client*,wl_resource*,uint32_t){assert(false);}};
  wl_resource_set_implementation(r,&api,data,nullptr);
 }
 static void Wm(wl_client* c,void* data,uint32_t,uint32_t id) {
  auto* r=wl_resource_create(c,&xdg_wm_base_interface,1,id);
  static const struct xdg_wm_base_interface api{Destroy,[](wl_client*,wl_resource*,uint32_t){assert(false);},
   [](wl_client* c,wl_resource* parent,uint32_t id,wl_resource*){
    auto& s=Get(parent);auto* r=wl_resource_create(c,&xdg_surface_interface,1,id);s.shell=r;
    static const struct xdg_surface_interface api{Destroy,
     [](wl_client* c,wl_resource* parent,uint32_t id){auto& s=Get(parent);auto* r=wl_resource_create(c,&xdg_toplevel_interface,1,id);s.top=r;
      static const struct xdg_toplevel_interface api=[] {
       struct xdg_toplevel_interface x{};x.destroy=Destroy;x.set_parent=[](wl_client*,wl_resource*,wl_resource*){};
       x.set_title=[](wl_client*,wl_resource*,const char*){};x.set_app_id=x.set_title;
       x.set_fullscreen=[](wl_client*,wl_resource*,wl_resource*){};x.unset_fullscreen=[](wl_client*,wl_resource*){};
       x.set_min_size=[](wl_client*,wl_resource*,int32_t,int32_t){};x.set_max_size=x.set_min_size;
       x.set_maximized=x.unset_fullscreen;x.unset_maximized=x.unset_fullscreen;x.set_minimized=x.unset_fullscreen;return x;
      }();wl_resource_set_implementation(r,&api,&s,nullptr);
     },[](wl_client*,wl_resource*,uint32_t,wl_resource*,wl_resource*){assert(false);},
     [](wl_client*,wl_resource*,int32_t,int32_t,int32_t,int32_t){},[](wl_client*,wl_resource*,uint32_t){}};
    wl_resource_set_implementation(r,&api,&s,nullptr);
   },[](wl_client*,wl_resource*,uint32_t){}};
  wl_resource_set_implementation(r,&api,data,nullptr);
 }
 static void Seat(wl_client* c,void* data,uint32_t version,uint32_t id) {
  auto* r=wl_resource_create(c,&wl_seat_interface,std::min(version,5u),id);
  static const struct wl_seat_interface api{
   [](wl_client* c,wl_resource* parent,uint32_t id){auto& s=Get(parent);s.pointer=wl_resource_create(c,&wl_pointer_interface,5,id);static const struct wl_pointer_interface p{[](wl_client*,wl_resource*,uint32_t,wl_resource*,int32_t,int32_t){},Destroy};wl_resource_set_implementation(s.pointer,&p,&s,nullptr);if(s.keyboard)s.inputReady=true;},
   [](wl_client* c,wl_resource* parent,uint32_t id){auto& s=Get(parent);s.keyboard=wl_resource_create(c,&wl_keyboard_interface,5,id);static const struct wl_keyboard_interface k{Destroy};wl_resource_set_implementation(s.keyboard,&k,&s,nullptr);if(s.pointer)s.inputReady=true;},
   [](wl_client*,wl_resource*,uint32_t){assert(false);},Destroy};
  wl_resource_set_implementation(r,&api,data,nullptr);wl_seat_send_capabilities(r,WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD);
 }
 static int Command(int fd,uint32_t,void* data) {
  auto& s=*static_cast<Server*>(data);char command;assert(read(fd,&command,1)==1);
  if(command=='i') {
   assert(s.pointer&&s.keyboard&&s.surface);
   wl_pointer_send_enter(s.pointer,++s.serial,s.surface,wl_fixed_from_int(20),wl_fixed_from_int(10));
   wl_pointer_send_motion(s.pointer,1,wl_fixed_from_int(20),wl_fixed_from_int(10));
   wl_array keys;wl_array_init(&keys);wl_keyboard_send_enter(s.keyboard,++s.serial,s.surface,&keys);wl_array_release(&keys);
   wl_keyboard_send_key(s.keyboard,++s.serial,1,30,WL_KEYBOARD_KEY_STATE_PRESSED);
   wl_keyboard_send_key(s.keyboard,++s.serial,2,30,WL_KEYBOARD_KEY_STATE_RELEASED);
   wl_pointer_send_button(s.pointer,++s.serial,2,0x110,WL_POINTER_BUTTON_STATE_PRESSED);
   wl_pointer_send_button(s.pointer,++s.serial,3,0x110,WL_POINTER_BUTTON_STATE_RELEASED);
   wl_pointer_send_axis(s.pointer,4,WL_POINTER_AXIS_VERTICAL_SCROLL,wl_fixed_from_int(10));
   wl_pointer_send_axis_discrete(s.pointer,WL_POINTER_AXIS_VERTICAL_SCROLL,1);wl_pointer_send_frame(s.pointer);
  } else if(command=='c'){assert(s.top);xdg_toplevel_send_close(s.top);}
  else if(command=='q')wl_display_terminate(s.display);
  wl_display_flush_clients(s.display);return 0;
 }
};
}
int main() {
 int pipes[2];assert(!pipe(pipes));Server server;server.display=wl_display_create();assert(server.display);
 assert(wl_global_create(server.display,&wl_compositor_interface,4,&server,Server::Compositor));
 assert(wl_global_create(server.display,&xdg_wm_base_interface,1,&server,Server::Wm));
 assert(wl_global_create(server.display,&wl_seat_interface,5,&server,Server::Seat));
 const char* socket=wl_display_add_socket_auto(server.display);assert(socket);assert(!setenv("WAYLAND_DISPLAY",socket,1));
 auto* source=wl_event_loop_add_fd(wl_display_get_event_loop(server.display),pipes[0],WL_EVENT_READABLE,Server::Command,&server);assert(source);
 std::thread thread([&]{wl_display_run(server.display);});
 int screenIndex=0;xcb_connection_t* x=xcb_connect(nullptr,&screenIndex);assert(x&&!xcb_connection_has_error(x));
 auto iterator=xcb_setup_roots_iterator(xcb_get_setup(x));for(int i=0;i<screenIndex;++i)xcb_screen_next(&iterator);auto* screen=iterator.data;
 xcb_window_t window=xcb_generate_id(x);uint32_t mask=XCB_EVENT_MASK_KEY_PRESS|XCB_EVENT_MASK_KEY_RELEASE|XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE|XCB_EVENT_MASK_STRUCTURE_NOTIFY;
 xcb_create_window(x,screen->root_depth,window,screen->root,0,0,320,240,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_EVENT_MASK,&mask);
 auto* atom=xcb_intern_atom_reply(x,xcb_intern_atom(x,0,16,"WM_DELETE_WINDOW"),nullptr);assert(atom);auto closeAtom=atom->atom;free(atom);
 auto* protocols=xcb_intern_atom_reply(x,xcb_intern_atom(x,0,12,"WM_PROTOCOLS"),nullptr);assert(protocols);
 xcb_change_property(x,XCB_PROP_MODE_REPLACE,window,protocols->atom,XCB_ATOM_ATOM,32,1,&closeAtom);free(protocols);
 xcb_map_window(x,window);xcb_flush(x);
 {
  steamohos::NativeX11Window bridge;assert(bridge.Create(window));assert(bridge.display&&bridge.surface);
  // The fullscreen host output is independent of the game's drawable. A
  // compositor configure must not resize X11 underneath its Vulkan swapchain.
  auto* geometry=xcb_get_geometry_reply(x,xcb_get_geometry(x,window),nullptr);assert(geometry);
  assert(geometry->width==320&&geometry->height==240);free(geometry);
  auto readyDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!server.inputReady&&std::chrono::steady_clock::now()<readyDeadline)poll(nullptr,0,1);
  assert(server.inputReady);
  assert(write(pipes[1],"i",1)==1);
  unsigned keyPress=0,keyRelease=0,left=0,wheel=0;bool closed=false;
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  assert(write(pipes[1],"c",1)==1);
  while(std::chrono::steady_clock::now()<deadline) {
   while(auto* event=xcb_poll_for_event(x)) {
    auto type=event->response_type&0x7f;
    if(type==XCB_KEY_PRESS){assert(reinterpret_cast<xcb_key_press_event_t*>(event)->detail==38);++keyPress;}
    if(type==XCB_KEY_RELEASE)++keyRelease;
    if(type==XCB_BUTTON_PRESS){auto detail=reinterpret_cast<xcb_button_press_event_t*>(event)->detail;if(detail==1)++left;else if(detail==5)++wheel;else assert(false);}
    if(type==XCB_CLIENT_MESSAGE)closed=reinterpret_cast<xcb_client_message_event_t*>(event)->data.data32[0]==closeAtom;
    free(event);
   }
   if(keyPress==1&&keyRelease==1&&left==1&&wheel==1&&closed)break;
   pollfd fd{xcb_get_file_descriptor(x),POLLIN,0};poll(&fd,1,10);
  }
  assert(keyPress==1&&keyRelease==1&&left==1&&wheel==1&&closed);
 }
 xcb_destroy_window(x,window);xcb_disconnect(x);
 assert(write(pipes[1],"q",1)==1);thread.join();wl_event_source_remove(source);
 wl_display_destroy_clients(server.display);wl_display_destroy(server.display);close(pipes[0]);close(pipes[1]);
 puts("PASS: real XTEST key/button/scroll without duplicate steps, Wayland configure, close and cleanup");
}
