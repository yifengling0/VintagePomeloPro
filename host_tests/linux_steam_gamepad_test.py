"""Exercise GNU controller transport and SDL3 ABI with a recording SDL fixture."""
from pathlib import Path
import os,socket,struct,subprocess,tempfile,threading,unittest
ROOT=Path(__file__).resolve().parents[1]
FIXTURE=r"""
#include "sdl3_virtual_abi.h"
#include <stdatomic.h>
#include <stdint.h>
static HmosVirtualJoystickDesc desc;
static atomic_int attached,axes[6],buttons[11],hat;
uint32_t SDL_WasInit(uint32_t flags){(void)flags;return 0x200;}
uint32_t SDL_AttachVirtualJoystick(const HmosVirtualJoystickDesc *d){
 if(d->version!=136||d->type!=1||d->naxes!=6||d->nbuttons!=11||d->nhats!=1)return 0;
 desc=*d;attached=1;return 42;
}
void *SDL_OpenJoystick(uint32_t id){return id==42?&desc:0;}
void SDL_CloseJoystick(void *p){(void)p;}
bool SDL_DetachVirtualJoystick(uint32_t id){(void)id;attached=0;return true;}
bool SDL_SetJoystickVirtualAxis(void *p,int n,int16_t v){(void)p;axes[n]=v;return true;}
bool SDL_SetJoystickVirtualButton(void *p,int n,bool v){(void)p;buttons[n]=v;return true;}
bool SDL_SetJoystickVirtualHat(void *p,int n,uint8_t v){(void)p;(void)n;hat=v;return true;}
int TestReady(void){return attached;}
int TestInput(void){return axes[0]==1234&&axes[1]==-2345&&axes[2]==-321&&axes[3]==32767&&axes[4]==-32768&&axes[5]==32767&&buttons[0]&&buttons[9]&&buttons[5]&&hat==3;}
int TestRumble(void){return desc.Rumble(0,123,456);}
int TestNeutral(void){return axes[0]==0&&axes[1]==0&&axes[4]==-32768&&buttons[0]==0&&hat==0;}
"""
CLIENT=r"""
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
int main(int argc,char **argv){if(argc!=2)return 1;void *lib=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);if(!lib)return 2;
 int (*ready)(void)=dlsym(lib,"TestReady"),(*input)(void)=dlsym(lib,"TestInput"),(*rumble)(void)=dlsym(lib,"TestRumble"),(*neutral)(void)=dlsym(lib,"TestNeutral");
 for(int i=0;i<60&&!ready();i++)usleep(50000);if(!ready())return 3;
 for(int i=0;i<60&&!input();i++)usleep(50000);if(!input())return 4;
 if(!rumble())return 5;
 for(int i=0;i<60&&!neutral();i++)usleep(50000);if(!neutral())return 6;
 puts("PASS WHGP fragmented state, SDL3 gamepad, axes/buttons/hat, rumble and disconnect neutral");return 0;}
"""
class NativeGamepad(unittest.TestCase):
 def test_wire_to_sdl(self):
  with tempfile.TemporaryDirectory(prefix='hmos-pad-') as temporary:
   base=Path(temporary);steam=base/'steamrtarm64';steam.mkdir();src=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'
   common=['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(src)]
   subprocess.run([*common,'-shared','-fPIC',str(src/'sdl_gamepad.c'),'-ldl','-pthread','-o',str(base/'bridge.so')],check=True)
   (base/'fixture.c').write_text(FIXTURE);(base/'client.c').write_text(CLIENT)
   subprocess.run([*common,'-shared','-fPIC',str(base/'fixture.c'),'-o',str(steam/'libSDL3.so.0')],check=True)
   subprocess.run([*common,'-Wno-misleading-indentation',str(base/'client.c'),'-ldl','-o',str(steam/'steam')],check=True)
   host=socket.socket(socket.AF_UNIX);host.bind(str(base/'whgp'));host.listen(1);host.settimeout(6);failures=[]
   def send():
    try:
     peer,_=host.accept();peer.settimeout(5)
     with peer:
      state=struct.pack('<IhhhhHHbb2x',(1<<0)|(1<<4)|(1<<14),1234,2345,-321,-32768,0,32767,1,1)
      message=struct.pack('<IHHII',0x50474857,2,1,0,len(state))+state
      peer.sendall(message[:3]);peer.sendall(message[3:17]);peer.sendall(message[17:])
      data=b''
      while len(data)<24:data+=peer.recv(24-len(data))
      self.assertEqual(struct.unpack('<IHHIIHHI',data),(0x50474857,2,3,0,8,123,456,0))
    except BaseException as e:failures.append(repr(e))
   worker=threading.Thread(target=send,daemon=True);worker.start()
   try:
    result=subprocess.run([str(steam/'steam'),str(steam/'libSDL3.so.0')],env={**os.environ,'LD_PRELOAD':str(base/'bridge.so'),'STEAMOHOS_CODE_ROOT':str(base),'STEAMOHOS_EXEC_PATH':str(steam/'steam'),'STEAMOHOS_GAMEPAD_SOCKET':str(base/'whgp')},capture_output=True,text=True,timeout=12)
    worker.join(7);self.assertFalse(worker.is_alive());self.assertFalse(failures,failures);self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout,end='')
   finally:host.close()
if __name__=='__main__':unittest.main()
