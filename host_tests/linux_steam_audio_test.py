"""Run a real ALSA plugin against the existing AudioBroker wire/ring contract."""
import array,ctypes,json,mmap,os,socket,struct,subprocess,tempfile,threading,time,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CLIENT=r"""
#include <alsa/asoundlib.h>
#include <stdio.h>
int main(int argc,char **argv) {
 (void)argv;snd_pcm_t *pcm=NULL;int err=snd_pcm_open(&pcm,"default",SND_PCM_STREAM_PLAYBACK,0);
 if(err<0)return argc>1?0:1;
 err=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,2,48000,0,80000);
 if(err<0){snd_pcm_close(pcm);return argc>1?0:2;}
 if(argc>1){snd_pcm_close(pcm);return 3;}
 short data[960];for(int i=0;i<480;i++){data[i*2]=1234;data[i*2+1]=-2345;}
 for(int i=0;i<20;i++)if(snd_pcm_writei(pcm,data,480)!=480){snd_pcm_close(pcm);return 4;}
 err=snd_pcm_drain(pcm);snd_pcm_close(pcm);return err<0?5:0;
}
"""
class AudioBridgeTest(unittest.TestCase):
 def test_alsa_broker_playback_and_failures(self):
  with tempfile.TemporaryDirectory(prefix='hmosdeck-audio-') as temp:
   base=Path(temp);src=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'
   subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-DPIC','-shared','-fPIC',str(src/'alsa_hmosdeck.c'),'-lasound','-o',str(base/'libasound_module_pcm_hmosdeck.so')],check=True)
   (base/'client.c').write_text(CLIENT)
   subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(base/'client.c'),'-lasound','-o',str(base/'client')],check=True)
   (base/'relay.cpp').write_text('#include "linux_steam/audio_bootstrap_relay.h"\n#include <cstdlib>\n#include <iostream>\nint main(int c,char**v){if(c!=3)return 1;steamohos::AudioBootstrapRelay r;if(!r.Start(atoi(v[1]),v[2]))return 2;std::cout<<r.Path()<<std::endl;std::cin.get();return 0;}')
   subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'entry/src/main/cpp'),str(base/'relay.cpp'),str(src.parent/'audio_bootstrap_relay.cpp'),'-pthread','-o',str(base/'relay')],check=True)
   env={**os.environ,'ALSA_PLUGIN_DIR':str(base),'ALSA_CONFIG_PATH':str(src/'hmosdeck-asound.conf')}
   env.pop('STEAMOHOS_AUDIO_BOOTSTRAP_FD',None)
   missing=subprocess.run([str(base/'client'),'expect-failure'],env=env,capture_output=True,timeout=10)
   self.assertEqual(missing.returncode,0,missing.stderr)
   for malformed,named in [(False,False),(True,False),(False,True),(True,True)]:
    host,guest=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET);host.settimeout(8)
    failures=[];received=[];commands=[]
    def serve():
     mem=None;view=None;control=None
     try:
      req,ancillary,_,_=host.recvmsg(512,socket.CMSG_SPACE(16))
      self.assertEqual(struct.unpack_from('IHH',req),(0x57484142,1,1))
      descriptors=array.array('i');descriptors.frombytes(ancillary[0][2]);control=socket.socket(fileno=descriptors[0]);control.settimeout(.005)
      started=False
      while True:
       if view and started:
        r,w=struct.unpack_from('QQ',view,32);w=min(w,r+240)
        for i in range(r,w):received.append(struct.unpack_from('hh',view,72+(i%8192)*4))
        struct.pack_into('Q',view,32,w)
       try:packet=control.recv(512)
       except socket.timeout:continue
       if not packet:break
       magic,version,cmd,seq,size=struct.unpack_from('IHHII',packet);self.assertEqual(magic,0x57484143);commands.append(cmd)
       fd=None
       if cmd==1:body=struct.pack('iIIII',0,1,48000,2,1)
       elif cmd==2:
        mem=os.memfd_create('hmosdeck-audio-ring');os.ftruncate(mem,72+8192*4);view=mmap.mmap(mem,72+8192*4)
        struct.pack_into('IIIIIIIIQQIIIIII',view,0,0 if malformed else 0x57484152,1,48000,2,1,4,8192,0,0,0,1,0,0,0,0,0)
        body=struct.pack('iIIIIIIII',0,1,48000,2,1,4,8192,len(view),256);fd=mem
       else:
        body=struct.pack('iI',0,1)
        if cmd==3:started=True;struct.pack_into('I',view,48,2)
        if cmd==4:started=False;struct.pack_into('I',view,48,1)
        if cmd==5:struct.pack_into('QQ',view,32,0,0)
       response=struct.pack('IHHII',magic,version,cmd,seq,len(body))+body
       control.sendmsg([response],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[fd]))] if fd is not None else [])
     except BaseException as e:failures.append(repr(e))
     finally:
      if control:control.close()
      if view:view.close()
      if mem is not None:os.close(mem)
    worker=threading.Thread(target=serve,daemon=True);worker.start()
    relay=None;client_env={**env};inherited=[]
    if named:
     relay=subprocess.Popen([str(base/'relay'),str(guest.fileno()),str(base)],pass_fds=[guest.fileno()],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
     client_env['STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET']=relay.stdout.readline().strip()
    else:client_env['STEAMOHOS_AUDIO_BOOTSTRAP_FD']=str(guest.fileno());inherited=[guest.fileno()]
    result=subprocess.run([str(base/'client')]+(['expect-failure'] if malformed else []),env=client_env,pass_fds=inherited,capture_output=True,text=True,timeout=12)
    if relay:
     relay.communicate('x',timeout=5);self.assertEqual(relay.returncode,0)

    guest.close();worker.join(10);host.close()
    self.assertFalse(worker.is_alive());self.assertFalse(failures,failures);self.assertEqual(result.returncode,0,result.stderr)
    if not malformed:
     self.assertEqual(len(received),9600);self.assertEqual(set(received),{(1234,-2345)})
     self.assertTrue(all(cmd in commands for cmd in [1,2,3,4,5,6]),commands)
   print('PASS real ALSA playback, ring PCM/order, drain/close, missing broker and malformed ring')
if __name__=='__main__':unittest.main()
