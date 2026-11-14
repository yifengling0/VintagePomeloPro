"""Execute the production deferred-map and depth-family selection code.

Host visibility/capability tests; these do not qualify a physical GPU.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DxvkContracts(unittest.TestCase):
    def run_cpp(self, body):
        with tempfile.TemporaryDirectory(prefix='vpp-dxvk-contract-') as folder:
            cpp = Path(folder) / 'test.cpp'
            binary = Path(folder) / 'test'
            cpp.write_text(body)
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            cpp, '-o', binary], check=True, timeout=30)
            return subprocess.run([binary], capture_output=True, timeout=10)

    def test_single_use_publication_after_recording_before_first_use(self):
        text = (ROOT / 'thirdparty/dxvk/src/d3d11/d3d11_context_def.cpp').read_text()
        start = text.index('      auto bufferSlice = pBuffer->AllocSlice();')
        body = text[start:text.index('\n    } else {', start)]
        harness = r'''
#include <cassert>
#include <functional>
#include <vector>
struct Slice { int *mapPtr; };
struct Buffer {
    int cpu = 0;
    Slice AllocSlice() { return {&cpu}; }
    Buffer *GetBuffer() { return this; }
};
struct Mapped { void *pData; };
static bool precise;
static bool winehuaFlushDynamicMapped() { return precise; }
struct DxvkContext {
    int published = 0, visible = 0, flushes = 0, invalidates = 0;
    void flushMappedBuffer(Buffer *b, Slice s) {
        assert(s.mapPtr == &b->cpu); published = *s.mapPtr; ++flushes;
    }
    void invalidateBuffer(Buffer *b, Slice s) {
        assert(s.mapPtr == &b->cpu); visible = published; ++invalidates;
    }
};
static std::vector<std::function<void(DxvkContext *)>> commands;
static void EmitCs(std::function<void(DxvkContext *)> fn) { commands.push_back(fn); }
static void map(Buffer *pBuffer, Mapped *pMappedResource) {
/* BODY */
}
int main() {
    for (bool mode : {false, true}) {
        precise = mode; commands.clear();
        Buffer a, b; Mapped ma, mb; DxvkContext ctx;
        map(&a, &ma); *static_cast<int *>(ma.pData) = 0x13579;
        map(&b, &mb); *static_cast<int *>(mb.pData) = 0x24680;
        assert(!ctx.flushes && !ctx.invalidates); // recording is not execution
        commands[0](&ctx);
        assert(ctx.visible == (mode ? 0x13579 : 0));
        commands[1](&ctx);
        assert(ctx.visible == (mode ? 0x24680 : 0));
        assert(ctx.flushes == (mode ? 2 : 0) && ctx.invalidates == 2);
    }
}
'''
        result = self.run_cpp(harness.replace('/* BODY */', body))
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        old = body.replace('        if (winehuaFlushDynamicMapped())\n'
                           '          ctx->flushMappedBuffer(cDstBuffer, cPhysSlice);', '')
        # Keep the now-unused switch helper referenced; failure must be runtime.
        old += '\n      (void)winehuaFlushDynamicMapped;'
        self.assertNotEqual(self.run_cpp(harness.replace('/* BODY */', old)).returncode, 0)

    def test_depth_family_default_explicit_capabilities_and_failure(self):
        text = (ROOT / 'thirdparty/dxvk/src/dxgi/dxgi_format.cpp').read_text()
        start = text.index('    const bool preferD32')
        body = text[start:text.index('\n\n    // The aggregate feature bit', start)]
        formats = sorted(set(re.findall(r'\b(?:DXGI_FORMAT|VK_FORMAT)_[A-Z0-9_]+\b', body)))
        constants = '\n'.join(f'constexpr int {name}={i+1};' for i, name in enumerate(formats))
        harness = r'''
#include <cassert>
#include <set>
#include <stdexcept>
#include <string>
/* CONSTANTS */
using VkFormatFeatureFlags = unsigned;
struct Adapter { bool d24=true, d32=true; };
static std::string setting;
namespace env { static std::string getEnvVar(const char *) { return setting; } }
struct Logger { static void info(const char *) {} };
using DxvkError=std::runtime_error;
static std::set<int> remapped;
static bool CheckImageFormatSupport(Adapter *a, int f, VkFormatFeatureFlags features) {
    assert(features == (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
    return f == VK_FORMAT_D24_UNORM_S8_UINT ? a->d24 : a->d32;
}
static void RemapDepthFormat(int f, int target) {
    assert(target == VK_FORMAT_D32_SFLOAT_S8_UINT); remapped.insert(f);
}
static void choose(Adapter *adapter) {
/* BODY */
}
int main() {
    for (const char *value : {"", "0", "false", "1"}) {
        for (bool d24 : {false, true}) for (bool d32 : {false, true}) {
            setting=value; Adapter a{d24,d32}; remapped.clear();
            bool failed=false;
            try { choose(&a); } catch (const DxvkError &) { failed=true; }
            assert(failed == (setting == "1" && !d32));
            if (!failed) {
                const bool remap=setting == "1" || !d24;
                assert(remapped.size() == (remap ? 4u : 0u));
            } else assert(remapped.empty());
            assert(a.d24==d24 && a.d32==d32); // physical reports never change
        }
    }
}
'''
        result = self.run_cpp(harness.replace('/* CONSTANTS */', constants).replace('/* BODY */', body))
        self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == '__main__':
    unittest.main()
