"""Exercise the complete production OHOS Winebus against recording HID stubs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WINE = ROOT / 'thirdparty/wine-valve'

STUBS = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include "wine/list.h"
#include "winehua_gamepad_protocol.h"
typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t USHORT, USAGE;
typedef uint32_t UINT, ULONG, NTSTATUS;
typedef uintptr_t UINT_PTR;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL 1
#define STATUS_NOT_IMPLEMENTED 2
#define STATUS_NOT_SUPPORTED 3
#define STATUS_PENDING 4
#define HID_USAGE_PAGE_GENERIC 1
#define HID_USAGE_GENERIC_GAMEPAD 5
#define BUS_TYPE_USB 1
#define CONTAINING_RECORD(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
struct effect_params { int unused; };
typedef struct { USAGE UsagePage, Usage; } USAGE_AND_PAGE;
struct hid_device_state { BYTE report_buf[64]; USHORT report_len; };
struct unix_device {
    struct list entry;
    struct hid_device_state hid_device_state;
    int buttons[11], axes[6], hat_x, hat_y, reports;
};
struct hid_device_vtbl {
    void (*destroy)(struct unix_device *);
    NTSTATUS (*start)(struct unix_device *);
    void (*stop)(struct unix_device *);
    NTSTATUS (*haptics_start)(struct unix_device *, UINT, USHORT, USHORT, USHORT, USHORT);
    NTSTATUS (*haptics_stop)(struct unix_device *);
    NTSTATUS (*control)(struct unix_device *, USAGE);
    NTSTATUS (*gain)(struct unix_device *, BYTE);
    NTSTATUS (*effect_control)(struct unix_device *, BYTE, USAGE, BYTE);
    NTSTATUS (*effect_update)(struct unix_device *, BYTE, struct effect_params *);
};
struct device_desc {
    USHORT vid, pid, version;
    int input;
    unsigned uid, bus_type;
    BOOL is_gamepad;
    uint16_t manufacturer[64], product[64], serialnumber[64];
};
struct bus_event { int type; };
static struct unix_device *created[4];
static struct device_desc descriptions[4];
static int creates[4], removes[4];
static void *hid_device_create(const struct hid_device_vtbl *vtbl, size_t size) {
    (void)vtbl; return calloc(1,size);
}
static BOOL hid_device_begin_report_descriptor(struct unix_device *d, const USAGE_AND_PAGE *u) { return TRUE; }
static BOOL hid_device_add_gamepad(struct unix_device *d) { return TRUE; }
static BOOL hid_device_add_haptics(struct unix_device *d) { return TRUE; }
static BOOL hid_device_end_report_descriptor(struct unix_device *d) { return TRUE; }
static void hid_device_set_button(struct unix_device *d, ULONG i, BOOL v) { assert(i<11);d->buttons[i]=v; }
static void hid_device_set_abs_axis(struct unix_device *d, ULONG i, int32_t v) { assert(i<6);d->axes[i]=v; }
static void hid_device_set_hatswitch_x(struct unix_device *d, ULONG i, int32_t v) { assert(i==0);d->hat_x=v; }
static void hid_device_set_hatswitch_y(struct unix_device *d, ULONG i, int32_t v) { assert(i==0);d->hat_y=v; }
static BOOL bus_event_queue_device_created(struct list *q, struct unix_device *d, struct device_desc *desc) {
    unsigned slot=desc->uid-0x57484750u; assert(slot<4); created[slot]=d;
    descriptions[slot]=*desc; creates[slot]++; return TRUE;
}
static BOOL bus_event_queue_device_removed(struct list *q, struct unix_device *d) {
    for(unsigned i=0;i<4;i++) {
        if(created[i]==d){removes[i]++;return TRUE;}
    }
    assert(0);return FALSE;
}
static void bus_event_queue_input_report(struct list *q, struct unix_device *d, BYTE *buf, USHORT len) { d->reports++; }
static BOOL bus_event_queue_pop(struct list *q, struct bus_event *e) { return FALSE; }
static void bus_event_cleanup(struct bus_event *e) {}
static void bus_event_queue_destroy(struct list *q) {}
'''

MAIN = r'''
static struct whgp_state_v2 state_for(unsigned slot)
{
    struct whgp_state_v2 state={0};
    state.reserved[0]=WHGP_STATE_TOPOLOGY_VALID|WHGP_STATE_CONNECTED;
    state.buttons=1u<<slot;state.lx=1000+slot;state.ly=2000+slot;
    state.hat_y=1;return state;
}
static void packet(int fd, unsigned slot, struct whgp_state_v2 body)
{
    struct whgp_header header={WHGP_MAGIC,WHGP_VERSION,WHGP_MSG_STATE,slot,sizeof(body)};
    assert(write(fd,&header,sizeof(header))==sizeof(header));
    assert(write(fd,&body,sizeof(body))==sizeof(body));
}
int main(void)
{
    struct whgp_state_v2 state, disconnected={0};
    struct unix_device *previous;
    struct whgp_header header;
    struct whgp_rumble_v1 rumble;
    int peers[2];
    assert(create_virtual_gamepad(0));
    for(unsigned i=0;i<4;i++) {
        state=state_for(i); apply_slot_state(i,&state);
        assert(creates[i]==1 && created[i]->reports==0);
        assert(descriptions[i].uid==0x57484750u+i);
        assert(descriptions[i].serialnumber[3]=='1'+i);
        assert(ohos_device_start(created[i])==STATUS_SUCCESS);
        assert(created[i]->buttons[i] && created[i]->axes[0]==1000+(int)i);
        assert(created[i]->axes[1]==2000+(int)i && created[i]->hat_y==-1);
    }
    state=state_for(1);state.buttons=0;apply_slot_state(1,&state);
    assert(!created[1]->buttons[1] && created[0]->buttons[0] && created[2]->buttons[2]);

    disconnected.reserved[0]=WHGP_STATE_TOPOLOGY_VALID;
    previous=created[2];apply_slot_state(2,&disconnected);
    assert(removes[2]==1 && find_gamepad(2)==NULL);
    apply_slot_state(2,&disconnected);assert(removes[2]==1);
    state=state_for(2);apply_slot_state(2,&state);
    assert(creates[2]==2 && created[2]!=previous);
    ohos_device_stop(previous);free(previous);
    assert(find_gamepad(2)==impl_from_unix_device(created[2]));
    ohos_device_start(created[2]);assert(created[2]->buttons[2]);

    assert(socketpair(AF_UNIX,SOCK_STREAM,0,peers)==0);sock_fd=peers[0];
    state=state_for(3);state.buttons=1;packet(peers[1],3,state);process_one_message(sock_fd);
    assert(created[3]->buttons[0] && !created[3]->buttons[3]);
    assert(created[0]->buttons[0] && created[2]->buttons[2]);
    ohos_device_haptics_start(created[2],250,12,34,0,0);
    assert(read_exact(peers[1],&header,sizeof(header)) && header.slot==2);
    assert(read_exact(peers[1],&rumble,sizeof(rumble)) && rumble.low==12 && rumble.high==34);
    ohos_device_haptics_stop(created[1]);
    assert(read_exact(peers[1],&header,sizeof(header)) && header.slot==1);
    assert(read_exact(peers[1],&rumble,sizeof(rumble)) && !rumble.low && !rumble.high);

    // Truncated transports release every controller, not only P1.
    close(peers[1]);process_one_message(sock_fd);assert(sock_fd==-1);
    for(unsigned i=0;i<4;i++)assert(!created[i]->buttons[0]&&!created[i]->buttons[i]&&!created[i]->axes[0]);

    // Invalid slots and oversized messages are rejected before blocking on a body.
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,peers)==0);sock_fd=peers[0];
    header=(struct whgp_header){WHGP_MAGIC,WHGP_VERSION,WHGP_MSG_STATE,4,4097};
    assert(write(peers[1],&header,sizeof(header))==sizeof(header));
    process_one_message(sock_fd);assert(sock_fd==-1);close(peers[1]);
    state=state_for(4);apply_slot_state(4,&state);assert(creates[3]==1);
    // A legacy v2 host (zero reserved bytes) retains single-pad behavior.
    state=state_for(0);state.reserved[0]=0;apply_slot_state(0,&state);assert(created[0]->buttons[0]);
    for(unsigned i=0;i<4;i++){ohos_device_stop(created[i]);free(created[i]);}
    puts("PASS Winebus: distinct four-device identities, per-slot HID, cached startup, hotplug, delayed stop, rumble routing, disconnect and legacy v2");
}
'''


def main():
    source = (WINE / 'dlls/winebus.sys/bus_ohos.c').read_text()
    source = source[source.index('struct ohos_device\n'):]
    with tempfile.TemporaryDirectory(prefix='vpp-wine-multiple-gamepads-') as temporary:
        folder = Path(temporary)
        code = folder / 'test.c'
        code.write_text(STUBS + source + MAIN)
        executable = folder / 'test'
        subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-D__WINESRC__',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-Wno-unused-function', '-pthread', '-I' + str(WINE / 'include'),
                        '-I' + str(WINE / 'dlls/winebus.sys'), str(code), '-o', str(executable)],
                       check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == '__main__':
    main()
