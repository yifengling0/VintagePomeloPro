#!/usr/bin/env python3
"""Execute production specified-Ability identity and foreground/restore guards."""
from pathlib import Path
import os, shutil, subprocess
root = Path(__file__).resolve().parents[1]
def method(file, signature):
    source = (root / file).read_text()
    start = source.index(signature)
    return source[start:source.index('\n  }', start) + 4]
stage = method('entry/src/main/ets/entryability/WineAbilityStage.ets', '  onAcceptWant(').replace('(want: Want): string', '(want)')
manager = method('entry/src/main/ets/service/WineWindowManager.ets', '  private async foregroundWineWindow(').replace('private ', '').replace('(id: number, win: window.Window): Promise<void>', '(id, win)')

code = """
const assert = require('assert/strict');
class Stage { STAGE }
class Manager { MANAGER }

const hilog={info(){},warn(){}}; const DOMAIN=0, TAG='test';
let visible=0,activated=0;
const testNapi={setToplevelVisible(){visible++},activateToplevel(){activated++}};
async function main(){
 const s=new Stage();const want=id=>({abilityName:'WineWindowAbility',parameters:{toplevelId:id}});
 assert.equal(s.onAcceptWant(want(9)),s.onAcceptWant({...want(9),parameters:{toplevelId:9,title:'changed'}}));
 assert.notEqual(s.onAcceptWant(want(9)),s.onAcceptWant(want(10)));
 for(const id of [0,-1,NaN,Infinity,1.5,Number.MAX_SAFE_INTEGER+1,undefined]) assert.equal(s.onAcceptWant(want(id)),'invalid-wine-window');
 assert.equal(s.onAcceptWant({abilityName:'Other',parameters:{toplevelId:9}}),'invalid-wine-window');
 const m=new Manager(),win={};let count=0,resolve;
 m.windows=new Map([[9,win]]);m.destroyed=new Set();m.restoringAbilities=new Map();m.abilityContexts=new Map([[9,{}]]);
 m.uiAbilityContext={startAbility(w){count++;assert.equal(w.parameters.toplevelId,9);assert.equal(w.parameters.restoreWineWindow,true);return new Promise(r=>resolve=r)}};
 const a=m.foregroundWineWindow(9,win),b=m.foregroundWineWindow(9,win);assert.equal(count,1);resolve();await Promise.all([a,b]);assert.equal(m.restoringAbilities.size,0);
 await m.foregroundWineWindow(9,{});m.destroyed.add(9);await m.foregroundWineWindow(9,win);assert.equal(count,1);m.destroyed.clear();
 m.uiAbilityContext.startAbility=()=>Promise.reject(new Error('denied'));await assert.rejects(m.foregroundWineWindow(9,win));assert.equal(m.restoringAbilities.size,0);
 console.log('specified identity / deduplicated foreground / no recursive reactivation: PASS');
}
main().catch(e=>{console.error(e);process.exitCode=1});
""".replace('STAGE',stage).replace('MANAGER',manager)
node = os.environ.get('NODE') or shutil.which('node') or '/apps/harmony/tool/node/bin/node'
subprocess.run([node,'-e',code],check=True,timeout=20)

ability_source = (root/'entry/src/main/ets/entryability/WineWindowAbility.ets').read_text()
assert 'win.restore()' not in ability_source
assert 'onNewWant(' in ability_source
assert 'noteAbilityForeground(this.toplevelId, true)' in ability_source
assert 'noteAbilityForeground(this.toplevelId, false)' in ability_source
