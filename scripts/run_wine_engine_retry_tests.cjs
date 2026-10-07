// Execute the production failed-session path and its serialized restart.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require(process.argv[2]);
const source = fs.readFileSync(path.resolve(__dirname,
  '../entry/src/main/ets/service/WineEngineService.ets'), 'utf8');
function method(signature) {
  const start = source.indexOf('  ' + signature);
  assert(start >= 0, signature);
  return source.slice(start, source.indexOf('\n  }', start) + 4);
}
const EngineState = {STOPPED:'stopped', PREPARING:'preparing', SWITCHING:'switching', READY:'ready', ERROR:'error'};
const AppSettingsStore = {getInstance:()=>({getGlobalSettings:()=>({wineLanguage:'zh_CN'})})};
class Fields {
  context = {}; state = EngineState.ERROR; mode = 'desktop'; presentationMode = 'desktop';
  d3dBackend = 'dxvk_legacy'; activePrefixMode = 'reuse'; activeWineLanguage = 'zh_CN';
  hostGraphicsBackendOverride = ''; hostGraphicsBackend = 'dxvk_legacy';
  appliedHostGraphicsBackend = 'dxvk_legacy'; hostGraphicsLabExperiment = '';
  appliedHostGraphicsLabExperiment = ''; restartPromise = null;
  selectedD3DBackend() {return 'dxvk_legacy';}
  setState(state) {this.state = state;}
  waitForReady() {throw new Error('Dead coordinator must never be awaited');}
}
eval(ts.transpileModule('class RetryService extends Fields {\n' +
  ['async startSession(', 'async restart(', 'private async performRestart('].map(method).join('\n') +
  '\n}\nglobalThis.RetryService = RetryService;',
  {compilerOptions:{target:ts.ScriptTarget.ES2020}}).outputText);
(async () => {
  for (const prefix of ['reuse', 'clean']) {
    const s = new RetryService();
    let drained; const drain = new Promise(resolve=>{drained=resolve;});
    let stops = 0, starts = 0;
    s.stopSession = async () => {++stops; await drain; s.state=EngineState.STOPPED;};
    s.startSession = async (mode,presentation,home,actualPrefix) => {
      ++starts; assert.equal(stops,1); assert.equal(s.state,EngineState.STOPPED);
      assert.deepEqual([mode,presentation,home,actualPrefix],['desktop','desktop','/games',prefix]);
      return true;
    };
    const ready = RetryService.prototype.startSession.call(s,'desktop','desktop','/games',prefix);
    assert.equal(stops,1,'Failure stops residual services before retry');
    assert.equal(starts,0,'New Wine cannot start before native drain');
    const concurrent = s.restart('desktop','desktop','/games',prefix);
    assert.equal(stops,1,'Concurrent retry shares the existing stop operation');
    drained();
    assert.equal(await ready,true); assert.equal(await concurrent,true);
    assert.equal(starts,1); assert.equal(s.restartPromise,null);
  }
  const failure = new RetryService();
  failure.stopSession = async () => {failure.state=EngineState.STOPPED;};
  failure.startSession = async () => false;
  assert.equal(await RetryService.prototype.startSession.call(failure,'desktop','desktop','/games'),false);
  assert.equal(failure.restartPromise,null,'A failed retry finishes instead of waiting for a nonexistent ready');
  console.log('PASS: failed-session recovery drains residual services, serializes retries, preserves clean/reuse prefix, and returns failure');
})().catch(error=>{console.error(error);process.exitCode=1;});
