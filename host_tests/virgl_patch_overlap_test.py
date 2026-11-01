"""The context-selection fix must be applied once in the ordered VirGL overlays."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
patch6 = (ROOT / 'patches/virglrenderer/0006-gl-present-producer-context.patch').read_text()
patch7 = (ROOT / 'patches/virglrenderer/0007-ohos-native-memory-context.patch').read_text()
build = (ROOT / 'scripts/apply_virglrenderer_ohos_patches.sh').read_text()
fix = 'virgl_renderer_submit_cmd(NULL, ctx->ctx_id, 0);'
assert patch6.count(fix) == 1
assert patch7.count(fix) == 0
order = re.findall(r'for name in (.+); do', build)[0]
assert order.index('0006-gl-present-producer-context.patch') < order.index(
    '0007-ohos-native-memory-context.patch')
print('VirGL producer-context fix is applied once before native-memory negotiation')
