#!/usr/bin/env node
// Host-only contract regression. It does not execute the native library or Wine.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2]);
const root = path.resolve(__dirname, '..');
const read = p => fs.readFileSync(path.join(root, p), 'utf8');
const declaration = read('entry/src/main/cpp/types/libentry/Index.d.ts');
const bridge = read('entry/src/main/cpp/bridge/napi_init.cpp');
const wine = read('entry/src/main/cpp/wine/wine_exe.cpp');
const added = ['initAppLog','clearNativeLog','controllerSetEnabled','controllerSetButton','controllerSetStick',
 'controllerSetHat','controllerResetSource','controllerSetOutputMode','controllerGetOutputMode','controllerGetStateText',
 'setHostGraphicsExperimentForLab','setHostGraphicsBackend','getHostGpuName','getDesktopRootId','getWineSession',
 'activateWineSession','stopWineSession','initGameController','isGamepadConnected','setGamepadButtonCallback',
 'setGamepadAxisCallback','setGamepadDeviceCallback','setGamepadRumbleCallback','refreshRenderer','setRendererPaused',
 'getDisplayFps','readPerformanceCounters','wineTextInputCommit','wineTextInputPreedit','wineTextInputEnabled',
 'wineTextInputSetArmed','runHostVulkanProbe','stopHostVulkanProbe'];
for (const name of added) {
 assert.ok(bridge.includes('"'+name+'"'), `C++ export ${name}`);
 assert.ok(declaration.includes('export const '+name+':'), `declaration ${name}`);
}
assert.ok(!/\bany\b|\bunknown\b/.test(declaration));
assert.match(wine,/size_t argc = 10;/);
assert.match(declaration,/environment\?: string\[\], wineLang\?: string\) => WineLaunchResult/);
assert.match(declaration,/getWineSession: \(sessionId: string\) => null/);
assert.match(declaration,/readPerformanceCounters: \(appCpu: boolean, systemCpu: boolean\) => Promise<string>/);
const options = /export interface WineProgramOptions \{([\s\S]*?)\n\}/.exec(declaration)[1];
for (const ignored of ['prefixMode','wineLang','presentToSurface','automationMode','dxvkBackend']) {
 assert.ok(!options.includes(ignored), `unsupported option ${ignored}`);
}
assert.ok(options.includes('presentBackend?: string;'));
const calls = [], summaries = [];
let syncCalls=0, restoreCalls=0;
const imports = {
 'libentry.so': { default: {runWineProgram: value => {calls.push(value); return {pid: 123};}} },
 './LogService': { hilog: {info(){},warn(){},error(){}} },
 './WineEngineService': {WineEngineService: {getInstance:()=>({isReady:()=>true,getD3dBackend:()=> 'dxvk_legacy',
   restoreProductSession:async()=>{restoreCalls++;return true;}})}},
 './ManagedSmokePayloadService': {
   smokeDriveC: prefix=>'/test/'+prefix+'/drive_c',ensureSmokeDirectory:()=>true,
   syncManagedSmoke:()=>{syncCalls++;return true;},readSmokeText:()=>JSON.stringify({status:'PASS'}),
   writeSmokeText:(_path,value)=>summaries.push(JSON.parse(value))
 }
};
const source = read('entry/src/main/ets/service/SmokeRunner.ets');
const js = ts.transpileModule(source, {compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText;
const moduleObject={exports:{}};
vm.runInNewContext(js,{module:moduleObject,exports:moduleObject.exports,
 require:name=>{assert.ok(imports[name], name);return imports[name];},setTimeout:callback=>{callback();return 0;}});
const runner = moduleObject.exports.SmokeRunner.getInstance();
const request={runId:'contract',suite:'core',prefix:'reuse',source:'unit'};
(async()=>{
 await runner.run({...request,prefix:'clean'});
 assert.equal(summaries.at(-1).status,'UNSUPPORTED');
 assert.equal(summaries.at(-1).tests[0].stage,'native-contract');
 assert.equal(calls.length,0);assert.equal(syncCalls,0);assert.equal(restoreCalls,0);
 for(const d3dBackend of ['vkd3d_limited_500k','invalid']) {
  assert.equal((await runner.runTest(request,{testId:'bad',exe:'test.exe',d3dBackend},'/test')).status,'UNSUPPORTED');
 }
 assert.equal((await runner.runTest(request,{testId:'bad',exe:'test.exe',d3dBackend:'dxvk_legacy',dxvkBackend:'dxvk_modern_2_6'},'/test')).status,'UNSUPPORTED');
 assert.equal(calls.length,0);
 for(const mode of ['offscreen','present']) {
  assert.equal((await runner.runTest(request,{testId:mode,exe:'test.exe',mode},'/test')).status,'PASS');
  const actual=calls.at(-1);
  assert.deepEqual(Object.keys(actual).sort(),['argv','d3dBackend','environment','windowsExePath','workingDirectory']);
  assert.equal(actual.d3dBackend,'dxvk_legacy');
  assert.ok(actual.argv.includes('--automation'));
  assert.ok(actual.argv.includes(mode==='offscreen'?'--offscreen':'--present'));
 }
 console.log('Native declarations + actual SmokeRunner: exports, ten-argument launch, unsupported clean/backend, supported argv PASS');
})().catch(error=>{console.error(error);process.exitCode=1;});
