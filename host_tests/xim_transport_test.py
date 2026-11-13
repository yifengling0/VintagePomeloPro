#!/usr/bin/env python3
"""Compile the real XIM receiver against wire packets from libX11's close sequence."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'entry/src/main/cpp/display/ncp/xim_server_child.cpp').read_text()
start=source.index('static void RxFeed(')
end=source.index('\nstatic void HandlePacket(',start)
receiver=source[start:end]
test=r"""
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
struct XimClient { std::string rx; bool lsb=true,lsb_known=true; };
static std::vector<unsigned> received;
static void HandlePacket(XimClient &,const std::string &p) { received.push_back((uint8_t)p[0]); }
RECEIVER
static std::string packet(unsigned opcode,unsigned body,bool little=true) {
 std::string p(4+body,'\0');p[0]=(char)opcode;
 unsigned words=body/4;p[little?2:3]=(char)words;p[little?3:2]=(char)(words>>8);return p;
}
int main() {
 for(bool little:{true,false}) {
  XimClient c;c.lsb=little;received.clear();
  auto close=packet(32,4,little);close[4]=1;
  auto disconnect=packet(3,0,little);
  auto input=close;input.resize(20,'\0');RxFeed(c,input.data(),input.size());
  assert(received==std::vector<unsigned>{32});assert(c.rx.empty());
  input=disconnect;input.resize(20,'\0');RxFeed(c,input.data(),input.size());
  assert((received==std::vector<unsigned>{32,3}));assert(c.rx.empty());
  for(int i=0;i<1000;i++) {
   RxFeed(c,close.data(),2);RxFeed(c,close.data()+2,close.size()-2);
   RxFeed(c,disconnect.data(),disconnect.size());
  }
  assert(received.size()==2002);assert(c.rx.empty());
  auto batch=disconnect+close+disconnect;RxFeed(c,batch.data(),batch.size());
  assert(received.size()==2005);assert(c.rx.empty());
 }
}
""".replace('RECEIVER',receiver)
with tempfile.TemporaryDirectory(prefix='xim-transport-') as d:
 p=Path(d);(p/'test.cpp').write_text(test)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS production XIM receiver: CLOSE/DISCONNECT, empty payload, CM padding, fragmented/coalesced and both byte orders, 2000 lifecycles')
