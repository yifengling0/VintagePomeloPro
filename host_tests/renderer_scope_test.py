"""Execute production renderer role and Direct consumer handoff without a GPU."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'entry/src/main/cpp/graphics/egl_renderer.cpp').read_text()
role = source[source.index('bool EglRenderer::RootCompositing() const {'):].strip()
assert role.endswith('}')
start = source.index('        // External Linux carriers own Wayland buffers')
handoff = source[start:source.index('        const bool paused =', start)]
unit = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include "direct/direct_consumer_scope.h"
static winehua::direct::DirectConsumerScopes owners;
static unsigned retired=0;
struct Compositor {
 struct PolicyState { bool desktop=true; bool RootCompositing() const {return desktop;} } policy;
 uint32_t root=0;
 const PolicyState& Policy() const {return policy;}
 uint32_t DesktopRootToplevelId() const {return root;}
};
namespace winehua::direct {
 struct DirectDesktopCompositor {
  DirectDesktopCompositor(Compositor&,int,uint32_t scope) {assert(owners.Register(this,scope));}
  ~DirectDesktopCompositor() {++retired;owners.Remove(this);}
 };
}
struct EglRenderer {
 Compositor& compositor_;uint32_t id=0;
 std::atomic<bool> isolatedToplevel_=false;
 std::unique_ptr<winehua::direct::DirectDesktopCompositor> directDesktop;
 uint32_t directScope=0;int display_=0;
 explicit EglRenderer(Compositor& c,uint32_t i):compositor_(c),id(i){}
 uint32_t GetToplevelId() const {return id;}
 bool RootCompositing() const;
 void Step() {const uint32_t identity=id;
 HANDOFF
 }
};
ROLE
int main() {
 Compositor c;
 EglRenderer steam(c,1);
 assert(!steam.RootCompositing());steam.Step();
 assert(owners.OwnerFor(1)!=nullptr&&owners.OwnerFor(2)==nullptr);
 // Broker ownership arrives after renderer creation, before lazy Wine startup.
 steam.isolatedToplevel_=true;steam.Step();
 assert(!owners.OwnerFor(1)&&retired==1);
 c.root=2;EglRenderer wine(c,2);
 assert(wine.RootCompositing()&&!steam.RootCompositing());wine.Step();
 auto* rootOwner=owners.OwnerFor(5);assert(rootOwner);
 for(unsigned i=0;i<100;++i)steam.Step();
 assert(owners.OwnerFor(5)==rootOwner&&!steam.directDesktop);
 // An old root renderer must release its global lease on demotion; the
 // managed window owns its scope until the broker isolates it for handoff.
 c.root=3;assert(!wine.RootCompositing());wine.Step();
 assert(owners.OwnerFor(2)&&!owners.OwnerFor(5));
 wine.isolatedToplevel_=true;wine.Step();
 EglRenderer replacement(c,3);replacement.Step();
 assert(replacement.RootCompositing()&&owners.OwnerFor(5));
 replacement.directDesktop.reset();
 c.policy.desktop=false;EglRenderer managed(c,7);managed.Step();
 assert(!managed.RootCompositing()&&owners.OwnerFor(7)&&!owners.OwnerFor(8));
 managed.id=8;managed.Step();
 assert(!owners.OwnerFor(7)&&owners.OwnerFor(8));
 std::puts("PASS production renderer scopes: native Steam, lazy Wine root, isolation, root replacement and managed windows");
}
'''.replace('HANDOFF', handoff).replace('ROLE', role)
with tempfile.TemporaryDirectory(prefix='renderer-scope-') as tmp:
    path=Path(tmp)
    (path/'test.cpp').write_text(unit)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                    '-I',str(root/'entry/src/main/cpp'),str(path/'test.cpp'),
                    '-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
