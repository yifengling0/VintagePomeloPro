"""Real socket/X11-auth contracts for native guest display connections."""
from pathlib import Path
import json, os, select, socket, struct, subprocess, tempfile
R=Path(__file__).resolve().parents[1]

def main():
 with tempfile.TemporaryDirectory(prefix='steamohos-native-x11-',dir='/var/tmp') as temp:
  task=Path(temp);source=task/'endpoint.cpp'
  source.write_text(r"""#include "native_x11_endpoint.h"
#include <cassert>
#include <cstdio>
int main(int n,char**v) {
 assert(n==2);steamohos::NativeX11Endpoint endpoint;assert(endpoint.Open(v[1]));
 sockaddr_in address{};socklen_t length=sizeof(address);
 assert(!getsockname(endpoint.fd,(sockaddr*)&address,&length));
 assert(address.sin_family==AF_INET&&ntohl(address.sin_addr.s_addr)==INADDR_LOOPBACK);
 assert(fcntl(endpoint.fd,F_GETFD)&FD_CLOEXEC);
 printf("%s\n%s\n",endpoint.display.c_str(),endpoint.authority.c_str());fflush(stdout);
 getchar();close(endpoint.fd);endpoint.fd=-1;puts("RELEASED");fflush(stdout);
 getchar();return 0;
}
""")
  subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I'+str(R/'entry/src/main/cpp/linux_steam'),str(source),'-o',str(task/'endpoint')],check=True)
  owner=subprocess.Popen([str(task/'endpoint'),str(task)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
  server=None
  try:
   display=owner.stdout.readline().strip();authority=Path(owner.stdout.readline().strip())
   assert display.startswith('127.0.0.1:') and authority.is_file()
   assert authority.stat().st_mode&0o777==0o600
   data=authority.read_bytes();records=[];offset=0
   while offset<len(data):
    family,=struct.unpack_from('!H',data,offset);offset+=2;fields=[]
    for _ in range(4):
     size,=struct.unpack_from('!H',data,offset);offset+=2;fields.append(data[offset:offset+size]);offset+=size
    records.append((family,fields))
   assert len(records)==2 and records[0][1][1]==b'57'
   assert records[1][1][1]==display.split(':')[1].encode()
   cookie=records[1][1][3];assert len(cookie)==16 and cookie!=bytes(16)
   assert records[0][1][3]==cookie and all(x[0]==65535 for x in records)
   port=int(display.split(':')[1])+6000
   with socket.create_connection(('127.0.0.1',port),3):pass
   owner.stdin.write('\n');owner.stdin.flush();assert owner.stdout.readline().strip()=='RELEASED'
   rd,wr=os.pipe()
   with (task/'xvfb.log').open('w') as log:
    server=subprocess.Popen(['Xvfb',':'+display.split(':')[1],'-displayfd',str(wr),'-screen','0','640x360x24','-nolisten','unix','-listen','tcp','-auth',str(authority)],pass_fds=[wr],stdout=log,stderr=log)
    os.close(wr)
    try:
     assert select.select([rd],[],[],10)[0], 'Xvfb readiness timeout'
     assert os.read(rd,64).strip()==display.split(':')[1].encode()
    finally:os.close(rd)
    def setup(token):
     name=b'MIT-MAGIC-COOKIE-1' if token else b''
     request=struct.pack('<BBHHHHH',ord('l'),0,11,0,len(name),len(token),0)
     request+=name+bytes((-len(name))%4)+token+bytes((-len(token))%4)
     with socket.create_connection(('127.0.0.1',port),3) as peer:
      peer.settimeout(3);peer.sendall(request);return peer.recv(8)[0]
    assert setup(b'')==0, 'Unauthenticated native connection accepted'
    assert setup(bytes(16))==0, 'Incorrect cookie accepted'
    assert setup(cookie)==1, 'Valid session cookie rejected'
  finally:
   if server:
    server.terminate();server.wait(timeout=5)
   owner.stdin.write('\n');owner.stdin.flush();owner.wait(timeout=5)
  assert not authority.exists()
  print('PASS: loopback-only endpoint, CLOEXEC, X11 cookie acceptance/rejection and cleanup')
if __name__=='__main__':main()
