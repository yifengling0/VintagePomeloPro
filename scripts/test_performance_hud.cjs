#!/usr/bin/env node
// Runs production pure models and the shared sampler with a fake scheduler/platform.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.env.TYPESCRIPT_PATH || '/apps/harmony/hvigor/hvigor/node_modules/typescript');
const root = path.resolve(__dirname, '../entry/src/main/ets');
const cache = new Map();
const timers = new Map();
let clock = 0, serial = 0, cpuCalls = 0, pending = [];
let lifecycle = null;
const preferences = new Map();
const store = {
  getSync: (key, fallback) => preferences.get(key) ?? fallback,
  put: async (key, value) => { preferences.set(key, value); }, flush: async () => {}
};
const mocks = {
  'libentry.so': { readPerformanceCounters: () => {
    cpuCalls++;
    return new Promise(resolve => pending.push(resolve));
  } },
  '@ohos.batteryInfo': { batteryTemperature: 420 },
  '@ohos.thermal': { getLevel: () => 2 },
  '@ohos.data.preferences': { getPreferences: async () => store },
  '@ohos.app.ability.ApplicationStateChangeCallback': {},
  '@kit.AbilityKit': {},
  'DeviceCapabilityPolicy': { DeviceCapabilityPolicy: {
    isTabletDevice: () => false, usesManagedWineWindows: () => false
  } }
};
function load(file) {
  if (cache.has(file)) return cache.get(file);
  const exports = {};
  cache.set(file, exports);
  const source = fs.readFileSync(file, 'utf8');
  const js = ts.transpileModule(source, { compilerOptions: {
    target: ts.ScriptTarget.ES2021, module: ts.ModuleKind.CommonJS
  } }).outputText;
  const localRequire = name => {
    const mock = mocks[name] || (name.endsWith('/DeviceCapabilityPolicy') ? mocks.DeviceCapabilityPolicy : null);
    if (mock) return { ...mock, default: mock };
    if (name.startsWith('.')) return load(path.resolve(path.dirname(file), name + '.ets'));
    throw Error('Unmocked dependency: ' + name);
  };
  vm.runInNewContext(js, {
    exports, require: localRequire, console,
    Date: { now: () => clock },
    AppStorage: { setOrCreate: () => {} },
    setInterval: (fn, ms) => { const id = ++serial; timers.set(id, { fn, ms }); return id; },
    clearInterval: id => timers.delete(id)
  }, { filename: file });
  return exports;
}
const model = name => load(path.join(root, 'model', name + '.ets'));
const settings = model('PerformanceHudSettings');
const metrics = model('PerformanceMetrics');
const clone = value => JSON.parse(JSON.stringify(value));
const snap = (timestampMs, cpuTicks = 0) => ({ timestampMs, ticksPerSecond: 100,
  appReadable: true, appPartial: false, systemReadable: true, systemPercent: -1,
  systemTotal: timestampMs, systemIdle: timestampMs / 2,
  processes: [{ pid: 42, startTicks: 1, cpuTicks }]
});
const settle = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); };
const resolveSample = async value => { pending.shift()(JSON.stringify(value)); await settle(); };
const tick = async () => { clock += 2000; for (const timer of [...timers.values()]) timer.fn(); await settle(); };

async function main() {
  const defaults = settings.normalizePerformanceHud();
  assert.equal(defaults.fps, true);
  assert.equal(defaults.appCpu, false);
  assert.equal(settings.normalizePerformanceHud({ appCpu: 'true', fps: 0 }).appCpu, false);
  const selected = settings.setPerformanceHudField(defaults, 'appCpu', true);
  assert.equal(defaults.appCpu, false);
  assert.equal(selected.appCpu, true);
  const empty = settings.setPerformanceHudField(defaults, 'fps', false);
  assert.equal(settings.hasPerformanceHudContent(empty), false);
  assert.equal(settings.hasPerformanceTelemetry(defaults), false);
  const removed = settings.normalizePerformanceHud({ ...empty, gpu: true, chipTemperature: true });
  assert.equal(settings.hasPerformanceHudContent(removed), false);
  assert.equal(Object.hasOwn(removed, 'gpu'), false);
  assert.equal(metrics.calculateCpuUsage(null, snap(1000)).app, null);
  const fallback = snap(1000); fallback.systemReadable = false; fallback.systemPercent = 23.4;
  assert.equal(metrics.calculateCpuUsage(null, fallback).system, 23.4);
  fallback.systemPercent = 0;
  assert.equal(metrics.calculateCpuUsage(null, fallback).system, null);
  assert.equal(metrics.calculateCpuUsage(snap(1000), snap(3000, 300)).app, 150);
  assert.equal(metrics.calculateCpuUsage(snap(1000), snap(3000, 300)).system, 50);
  assert.equal(metrics.calculateCpuUsage(snap(1000), snap(1001, 300)).app, null);
  assert.equal(metrics.calculateCpuUsage(snap(1000), snap(20000, 300)).app, null);
  const reused = snap(3000, 300); reused.processes[0].startTicks = 2;
  assert.equal(metrics.calculateCpuUsage(snap(1000), reused).app, null);
  const born = snap(3000, 100); born.processes.push({ pid: 43, startTicks: 2, cpuTicks: 400 });
  assert.equal(metrics.calculateCpuUsage(snap(1000), born).app, 50);
  assert.equal(metrics.calculateCpuUsage(snap(1000), born).partial, true);
  const denied = snap(3000); denied.appReadable = false;
  assert.equal(metrics.calculateCpuUsage(snap(1000), denied).app, null);
  assert.equal(metrics.formatCpuPercent(150, true), '≥150%');
  assert.equal(metrics.formatCpuPercent(NaN), '不可用');
  assert.equal(metrics.formatBatteryTemperature(420), '42.0℃');
  assert.equal(metrics.formatBatteryTemperature(42000), '不可用');
  assert.equal(metrics.formatThermalLevel(2), '温热 (2)');
  assert.equal(metrics.formatThermalLevel(9), '不可用');

  // Old JSON migration and every explicit settings-copy path preserve selections.
  preferences.set('global_settings_v1', JSON.stringify({ showFpsHud: true }));
  const { AppSettingsStore } = load(path.join(root, 'service/AppSettingsStore.ets'));
  const saved = AppSettingsStore.getInstance();
  await saved.initialize({});
  assert.equal(saved.getGlobalSettings().showFpsHud, true);
  assert.equal(saved.getGlobalSettings().wineLanguage, 'zh_CN');
  assert.deepEqual(clone(saved.getGlobalSettings().performanceHud), clone(defaults));
  const initial = clone(saved.getGlobalSettings()); initial.performanceHud = selected;
  initial.wineLanguage = 'en_US';
  await saved.saveGlobalSettings(initial);
  await saved.setViewMode(initial.viewMode);
  await saved.setDefaultDisplayMode(initial.defaultDisplayMode);
  await saved.setControlBarPosition(0.2, 0.3);
  await saved.setShowVirtualControls(false);
  await saved.setBox64Preset('default');
  assert.equal(JSON.parse(preferences.get('global_settings_v1')).performanceHud.appCpu, true);
  assert.equal(JSON.parse(preferences.get('global_settings_v1')).wineLanguage, 'en_US');
  const reloaded = new AppSettingsStore(); await reloaded.initialize({});
  assert.equal(reloaded.getGlobalSettings().performanceHud.appCpu, true);
  assert.equal(reloaded.getGlobalSettings().wineLanguage, 'en_US');

  const { PerformanceMonitorService } = load(path.join(root, 'service/PerformanceMonitorService.ets'));
  const service = PerformanceMonitorService.getInstance();
  const app = { on: (_, cb) => { lifecycle = cb; }, off: () => { lifecycle = null; } };
  const context = { getApplicationContext: () => app };
  let reading = null, foreground = true;
  const listener = (value, fg) => { reading = value; foreground = fg; };
  service.subscribe(context, listener, defaults);
  assert.equal(timers.size, 1); assert.equal(cpuCalls, 0); // FPS only never scans procfs.
  service.unsubscribe(listener); assert.equal(timers.size, 0); assert.equal(lifecycle, null);
  service.subscribe(context, listener, empty);
  assert.equal(timers.size, 0); assert.equal(cpuCalls, 0);
  service.unsubscribe(listener);
  service.subscribe(context, listener, selected);
  assert.equal(cpuCalls, 1);
  await resolveSample(snap(1000));
  assert.equal(reading.appCpu, '采样中');
  await tick(); await resolveSample(snap(3000, 300));
  assert.equal(reading.appCpu, '150%');
  await tick(); // In-flight request followed by background.
  lifecycle.onApplicationBackground();
  assert.equal(timers.size, 0); assert.equal(foreground, false);
  await resolveSample(snap(5000, 600));
  assert.equal(reading.appCpu, '采样中'); // Late result discarded.
  lifecycle.onApplicationForeground();
  await resolveSample(snap(20000, 800));
  assert.equal(reading.appCpu, '采样中'); // Resume resets CPU baseline.
  const listener2 = () => {};
  service.subscribe(context, listener2, selected);
  assert.equal(timers.size, 1); // Multiple windows share one timer.
  await resolveSample(snap(22000, 900));
  service.unsubscribe(listener2);
  service.unsubscribe(listener);
  while (pending.length) await resolveSample(snap(24000));
  assert.equal(timers.size, 0);
  const battery = settings.setPerformanceHudField(empty, 'batteryTemperature', true);
  service.subscribe(context, listener, battery);
  assert.equal(reading.batteryTemperature, '42.0℃');
  service.unsubscribe(listener);
  assert.equal(timers.size, 0);
  console.log('Performance HUD: settings migration/persistence, CPU deltas, sensor units, shared timer and lifecycle tests passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
