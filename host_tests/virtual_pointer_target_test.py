"""Replay a game appearing under a stationary cursor through production ArkTS."""
from pathlib import Path
import argparse
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--node', default='node')
p.add_argument('--typescript', default='typescript')
p.add_argument('--baseline', action='store_true')
args = p.parse_args()
path = 'entry/src/main/ets/components/VirtualInputOverlay.ets'
source = (subprocess.check_output(['git', 'show', 'HEAD:' + path], cwd=ROOT).decode()
          if args.baseline else (ROOT / path).read_text())

def method(signature):
    start = source.index('  private ' + signature)
    return source[start:source.index('\n  }', start) + 4]

fixture = '''class Overlay {
vcX=967;vcY=1141;cachedMoveTl=-1;cachedMoveTlX=-1e9;cachedMoveTlY=-1e9;cachedMoveTlAt=0;
fallbackToplevelId(){return 1;}
ensureScreenPx(){} syncCursorIndicator(){} resetTouchpadState(){}
applyCursorDelta(x,y){this.vcX+=x;this.vcY+=y;}
profile={cursorSpeed:1};vcInit=true;tpDown=false;tpMoved=false;tpDragArmed=false;
tpDragging=false;tpDragTl=0;tpTwoFinger=false;tpLastTapTime=0;
''' + '\n'.join(method(sig) for sig in ['tlAtCursor(', 'clickAtCursor(', 'handleTouchpad(']) + '''
static DOUBLE_TAP_MS=300;static DOUBLE_TAP_DIST=16;static DRAG_THRESHOLD=4;
static TWO_FINGER_TAP_MS=200;static SCROLL_STEP_PX=24;
}
globalThis.Overlay=Overlay;
globalThis.VirtualInputOverlay=Overlay;
'''
runner = '''const ts=require(TS_MODULE),assert=require('assert');
let clock=1000,target=9,queries=0,raised=[],input=[];
Date.now=()=>clock;
global.TouchType={Down:0,Move:1,Up:2,Cancel:3};
global.MouseAction={Press:1,Release:2,Move:3};
global.vp2px=x=>x;global.mapMouseButton=x=>x;
global.InputDispatcher={getInstance:()=>({rememberPointer(){}})};
global.testNapi={findToplevelAt:()=>{queries++;return target;},
  raiseToplevel:id=>raised.push(id),sendPointerEvent:(...args)=>input.push(args)};
eval(ts.transpileModule(FIXTURE,{compilerOptions:{target:ts.ScriptTarget.ES2020}}).outputText);
const m=new Overlay();
assert.equal(m.tlAtCursor(),9);assert.equal(queries,1);
// The new game covers Steam; the pointer has not moved. Pressing must resolve
// before the raise, otherwise raising cached Steam changes the native hit test.
target=18;clock+=5;m.clickAtCursor(1);
assert.equal(raised.at(-1),18,'stationary click must raise the newly visible game');
assert.equal(input.at(-2)[0],18);assert.equal(input.at(-1)[0],18);
assert.equal(input.at(-2)[1],1);assert.equal(input.at(-1)[1],2);
assert.equal(m.tlAtCursor(),18);const cachedQueries=queries;
clock+=5;m.tlAtCursor();assert.equal(queries,cachedQueries,'nearby moves can still coalesce');
target=20;clock+=40;assert.equal(m.tlAtCursor(),20,'moving cache must expire when the scene changes');
// Double-tap drag also resolves before raising; subsequent release stays paired.
target=21;clock+=5;m.tpLastTapTime=clock-50;m.tpLastTapVcX=m.vcX;m.tpLastTapVcY=m.vcY;
const event=(type,x,y)=>({type,touches:[{x,y}],changedTouches:[{x,y}]});
m.handleTouchpad(event(TouchType.Down,100,100));
m.handleTouchpad(event(TouchType.Move,104,100));
assert.equal(raised.at(-1),21,'drag press must refresh even for nearby motion');
assert.equal(m.tpDragTl,21);target=22;
m.handleTouchpad(event(TouchType.Up,104,100));
assert.equal(input.at(-1)[0],21,'release must remain paired with captured drag target');
// Expired/clock-reset cache cannot indefinitely pin a vanished window.
clock=1;assert.equal(m.tlAtCursor(),22);
console.log('PASS stationary click, drag, paired release, bounded move cache and clock reset');
'''.replace('TS_MODULE', json.dumps(args.typescript)).replace('FIXTURE', json.dumps(fixture))
with tempfile.TemporaryDirectory(prefix='vp-pointer-target-') as directory:
    script = Path(directory) / 'test.cjs'
    script.write_text(runner)
    result = subprocess.run([args.node, str(script)], capture_output=True, text=True)
    if args.baseline:
        assert result.returncode != 0 and 'stationary click must raise' in result.stderr, result.stdout + result.stderr
        print('BASELINE RED: stationary click raises cached Steam over the new game')
    else:
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout.strip())
