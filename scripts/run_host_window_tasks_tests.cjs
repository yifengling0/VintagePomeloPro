const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2]);
const output = {};
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(__dirname,
 '../entry/src/main/ets/service/HostWindowTasks.ets'),'utf8'), {
 compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}
}).outputText, {exports:output,Map,Set});
async function tests() {
 const tasks=output.HostWindowTasks.getInstance();
 const event=(id,kind,data={})=>tasks.onEvent(id,kind,JSON.stringify(data));
 let calls=[], release;
 tasks.setWineGameEligibility(id=>id==='game');
 tasks.setWineCloser(async (task,force)=>{calls.push([task.sessionId,force,task.key]);return true;});
 event(1,'created',{sessionId:'steam',clientPid:10});event(1,'title',{title:'Steam'});
 event(2,'created',{sessionId:'game',clientPid:20});event(2,'title',{title:'仙剑'});
 event(3,'modal',{modal:1,owner:2});event(3,'created',{sessionId:'game'});
 event(4,'desktop_root');event(4,'created',{sessionId:'desktop'});
 assert.equal(tasks.snapshot().length,2,'modal and desktop are excluded');
 assert.equal(tasks.focusTarget(2),3,'focus restores the modal child');
 assert.equal(tasks.controllableGames().length,1,'Steam infrastructure cannot be closed as a game');
 const key=tasks.controllableGames()[0].key;
 assert.equal(await tasks.controlGame(key,false),true);
 assert.deepEqual(calls[0].slice(0,2),['game',false]);
 event(2,'destroyed');event(2,'created',{sessionId:'other',clientPid:21});
 assert.equal(await tasks.controlGame(key,true),false,'recycled toplevel cannot inherit an old close action');
 tasks.associateSession(2,'game');
 const fresh=tasks.snapshot().find(t=>t.toplevelId===2).key;
 assert.notEqual(fresh,key);assert.equal(await tasks.controlGame(fresh,true),true);
 tasks.setActivator(async (_task,current)=>{await new Promise(r=>{release=r;});return current();});
 const restoring=tasks.activate(fresh);await Promise.resolve();
 tasks.markLauncherForeground();release();
 assert.equal(await restoring,false,'Home cancels an outstanding foreground restore');
 tasks.setActivator(async (_task,current)=>current());
 event(2,'minimized');assert.equal(await tasks.activate(fresh),true);
 assert.equal(tasks.snapshot().find(t=>t.toplevelId===2).minimized,false);
 const stale=tasks.activate(fresh);tasks.reset();
 assert.equal(await stale,false,'mode reset cancels stale activation');
 assert.equal(tasks.snapshot().length,0);
 console.log('OHOS Wine task lifecycle: owned close/force-end, Steam exclusion, recycled IDs, modal focus, Home and mode-change cancellation PASS');
}
tests().catch(e=>{console.error(e);process.exitCode=1;});