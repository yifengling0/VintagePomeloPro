'''Execute the production ArkTS service methods against both native protocols.

Uses the SDK TypeScript compiler, a controlled clock, and the actual overlay
detail method. No device/render claims: HAP and screenshots verify those.
'''
from pathlib import Path
import argparse
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--node', default='node')
parser.add_argument('--typescript', default='typescript')
args = parser.parse_args()
service = (ROOT / 'entry/src/main/ets/service/WineEngineService.ets').read_text()
overlay = (ROOT / 'entry/src/main/ets/components/WineEngineBlockingOverlay.ets').read_text()

def method(source, signature):
    start = source.index('  ' + signature)
    return source[start:source.index('\n  }', start) + 4]

methods = '\n'.join(method(service, name) for name in [
    'getPhaseText()', 'getPhaseElapsedMs()', 'getPreparationElapsedMs()',
    'private phaseText(', 'private handleNativeState(', 'private handleEnginePhase(',
    'private setState('])
source = '''class Service {
state = EngineState.STOPPED; message = ''; enginePhase = ''; phaseStartedAt = 0;
preparationStartedAt = 0; readySafetyTimer = -1; prefixRefreshRequired = false;
listeners = new Set(); waiters = []; pendingPrefixVersion = new Uint8Array(0);
resetReadySafetyNet() {} finishWaiters() {} syncManagedSmokePayload() {}
writeVersionMarker() { return true; } failReasonText(x) { return x; }
''' + methods + '\n}\nclass Overlay {\nstate; phaseText; elapsedSec;\n' + method(overlay, 'private detail(') + '\n}\nglobalThis.Service = Service; globalThis.Overlay = Overlay;'
javascript = '''const ts = require(TS_MODULE);
const assert = require('assert');
global.EngineState = { STOPPED:'stopped', PREPARING:'preparing', SWITCHING:'switching', READY:'ready', ERROR:'error' };
global.canTransitionEngineState = () => true;
global.hilog = {info(){}, warn(){}, error(){}};
global.DOMAIN=0; global.TAG='test'; global.testNapi={getProcessList:()=>[]};
let now=1000; Date.now=()=>now;
eval(ts.transpileModule(SOURCE, {compilerOptions:{target:ts.ScriptTarget.ES2020}}).outputText);
const s = new Service();
let observedPhase = '';
s.listeners.add(()=> { observedPhase = s.getPhaseText(); });
s.setState(EngineState.PREPARING, 'preparing runtime');
now += 3000;
assert.strictEqual(s.getPreparationElapsedMs(), 3000);
assert.strictEqual(s.getPhaseElapsedMs(), 3000);
const overlay = new Overlay();
overlay.state=EngineState.PREPARING; overlay.phaseText=''; overlay.elapsedSec=3;
assert(overlay.detail().includes('已 3 秒'), 'pre-native extraction has a clock');
s.handleNativeState('state:starting:wineserver');
assert.strictEqual(observedPhase, '启动 Wine 服务');
now += 2000;
s.handleNativeState('state:starting:wineserver');
assert.strictEqual(s.getPhaseElapsedMs(), 2000, 'duplicate stage does not reset clock');
assert.strictEqual(s.getPreparationElapsedMs(), 5000);
s.handleNativeState('state:starting:wineboot');
assert.strictEqual(s.getPhaseText(), '初始化 Wine 环境');
assert.strictEqual(s.getPhaseElapsedMs(), 0);
s.handleNativeState('phase:explorer');
assert.strictEqual(observedPhase, '启动桌面');
assert.strictEqual(s.getPreparationElapsedMs(), 5000, 'phase changes preserve total elapsed');
s.handleNativeState('phase:ready');
assert.strictEqual(s.state, EngineState.PREPARING, 'informational ready does not finish init');
s.handleNativeState('state:ready');
assert.strictEqual(s.state, EngineState.READY);
now += 1000;
s.setState(EngineState.SWITCHING, 'rebuild');
assert.strictEqual(s.getPreparationElapsedMs(), 0);
assert.strictEqual(s.getPhaseText(), '');
now += 4000;
overlay.state=EngineState.SWITCHING; overlay.phaseText=''; overlay.elapsedSec=4;
assert(overlay.detail().includes('已 4 秒'), 'rebuild has a visible clock');
s.setState(EngineState.STOPPED, 'stop');
now += 1000;
s.handleNativeState('phase:graphics');
assert.strictEqual(s.state, EngineState.PREPARING);
assert.strictEqual(observedPhase, '准备图形栈', 'first native phase reaches listeners');
assert.strictEqual(s.getPreparationElapsedMs(), 0);
now += 6000;
s.handleNativeState('phase:wineboot');
assert.strictEqual(s.getPreparationElapsedMs(), 6000, 'main protocol shares progress clock');
s.handleNativeState('state:failed:wineserver');
assert.strictEqual(s.state, EngineState.ERROR, 'failure still releases blocker');
s.setState(EngineState.PREPARING, 'retry');
assert.strictEqual(s.getPreparationElapsedMs(), 0, 'retry starts a new clock');
assert.strictEqual(s.getPhaseText(), '', 'retry clears old stage');
console.log('Wine engine progress PASS: both protocols, extraction, phases, duplicate events, rebuild, failure and retry');
'''
javascript = javascript.replace('TS_MODULE', json.dumps(args.typescript)).replace('SOURCE', json.dumps(source))
with tempfile.TemporaryDirectory(prefix='vp-progress-test-') as tmp:
    script = Path(tmp) / 'test.cjs'
    script.write_text(javascript)
    subprocess.run([args.node, str(script)], check=True)
