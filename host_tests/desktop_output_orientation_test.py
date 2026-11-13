"""Replay phone launcher/settings/surface ordering using production ArkTS methods."""
from pathlib import Path
import argparse
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--node', default='node')
parser.add_argument('--typescript', default='typescript')
parser.add_argument('--revision')
args = parser.parse_args()
path = 'entry/src/main/ets/service/WineWindowManager.ets'
source = subprocess.check_output(['git', 'show', f'{args.revision}:{path}'], cwd=ROOT).decode() if args.revision else (ROOT / path).read_text()

def method(signature):
    start = source.index('  ' + signature)
    return source[start:source.index('\n  }', start) + 4]

methods = '\n'.join(method(signature) for signature in [
    'setOutputScalePercent(', 'syncOutputSize(', 'prepareOutputForOrientation(',
    'desktopDisplaySafeSize(', 'private shouldNotifyDesktopToplevelResize('])
fixture = '''class Manager {
displayScale=1; scaleBoost=1; outputScalePercent=100;
outputHostWidth=0; outputHostHeight=0;
presentationMode=WinePresentationMode.DESKTOP; isDesktopMode=true; desktopRootId=0;
''' + methods + '\n}\nglobalThis.Manager=Manager;'
runner = '''const ts = require(TS_MODULE);
const assert = require('assert');
global.WinePresentationMode={DESKTOP:'desktop',MANAGED_WINDOWS:'managed'};
global.DesktopWindowMode={FULLSCREEN:'fullscreen',SAFE_AREA:'safe-area',FLOATING:'floating'};
global.ScreenOrientationSetting={PORTRAIT:'portrait',LANDSCAPE:'landscape',SENSOR_LANDSCAPE:'sensor-landscape'};
global.hilog={info(){},warn(){}}; global.DOMAIN=0; global.TAG='test';
let phone=true, managed=false;
const settings={desktopOrientation:'sensor-landscape',desktopWindowMode:'fullscreen'};
global.DeviceCapabilityPolicy={isPhoneDevice:()=>phone};
global.AppSettingsStore={getInstance:()=>({getGlobalSettings:()=>settings,usesManagedWineWindows:()=>managed})};
global.resolveCornerSafeMarginPx=()=>48;
let screen={width:1280,height:2832,densityPixels:3.5};
global.display={getDefaultDisplaySync:()=>screen};
let outputs=[], configures=[];
global.testNapi={setDisplayScale(){},setOutputSize:(w,h)=>outputs.push([w,h]),notifyToplevelResize:(id,w,h)=>configures.push([id,w,h])};
eval(ts.transpileModule(FIXTURE,{compilerOptions:{target:ts.ScriptTarget.ES2020}}).outputText);
const last=()=>outputs.at(-1);
const m=new Manager();
// Engine prepares landscape, then Index completes its asynchronous settings load
// while the phone display is still portrait. This is the captured failing order.
m.prepareOutputForOrientation(settings.desktopOrientation);
assert.deepStrictEqual(last(),[1416,640]);
m.setOutputScalePercent(100);
assert.deepStrictEqual(last(),[1416,640],'late unchanged Index settings must preserve prepared landscape output');
m.setOutputScalePercent(125);
assert.deepStrictEqual(last(),[1133,512],'a real scale change must reuse desktop geometry, not portrait library geometry');
m.setOutputScalePercent(100);
m.desktopRootId=7;
const before=outputs.length;
m.syncOutputSize(1280,2832);
assert.strictEqual(outputs.length,before,'transient portrait XComponent must not change landscape output');
assert.strictEqual(configures.length,0,'transient geometry must not configure the Wine root');
m.syncOutputSize(2832,1280);
assert.deepStrictEqual(last(),[1416,640]);
assert.deepStrictEqual(configures.at(-1),[7,1416,640]);
// A library resume/unrelated settings notification occurs after desktop creation.
screen={width:1280,height:2832,densityPixels:3.5};
m.setOutputScalePercent(100);
assert.deepStrictEqual(last(),[1416,640]);
settings.desktopOrientation='portrait';
m.prepareOutputForOrientation('portrait');
assert.deepStrictEqual(last(),[640,1416]);
const portraitCount=outputs.length;
m.syncOutputSize(2832,1280);
assert.strictEqual(outputs.length,portraitCount,'portrait desktop rejects transient landscape surface');
m.syncOutputSize(1280,2832);
assert.deepStrictEqual(last(),[640,1416]);
// Tablet and managed windows retain their existing resize behavior.
phone=false; settings.desktopOrientation='landscape';
m.syncOutputSize(1280,2832);
assert.deepStrictEqual(last(),[640,1416]);
phone=true; m.presentationMode='managed'; managed=true;
m.syncOutputSize(1280,2832);
assert.deepStrictEqual(last(),[640,1416]);
// Floating thumbnails never resize the root; explicit orientation preparation can.
managed=false; m.presentationMode='desktop'; settings.desktopWindowMode='floating';
configures=[]; m.syncOutputSize(2832,1280);
assert.strictEqual(configures.length,0);
m.prepareOutputForOrientation('sensor-landscape');
assert.deepStrictEqual(configures.at(-1),[7,1416,640]);
// Safe margins are applied once and remain unchanged when the scale changes.
settings.desktopWindowMode='safe-area'; m.prepareOutputForOrientation('landscape');
assert.deepStrictEqual(last(),[1368,640]);
m.setOutputScalePercent(125);
assert.deepStrictEqual(last(),[1094,512]);
console.log('Desktop output orientation PASS: captured startup race, scale updates, transient rotation, portrait, managed/tablet, floating and safe margins');
'''
runner = runner.replace('TS_MODULE', json.dumps(args.typescript)).replace('FIXTURE', json.dumps(fixture))
with tempfile.TemporaryDirectory(prefix='vp-orientation-test-') as tmp:
    script = Path(tmp) / 'test.cjs'
    script.write_text(runner)
    subprocess.run([args.node, str(script)], check=True)
