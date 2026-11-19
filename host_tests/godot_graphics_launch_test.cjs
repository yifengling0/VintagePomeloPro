// Exercise the production argument resolver. Run with the SDK TypeScript
// module path as argv[2], or with a locally installed typescript package.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const ts = require(process.argv[2] || 'typescript');
const source = fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/model/AppModels.ets'), 'utf8');
const start = source.indexOf('export const GRAPHICS_LAUNCH_PRESET_DEFAULT');
const end = source.indexOf('export interface VirtualMouseConfig', start);
assert(start >= 0 && end > start);
const compiled = ts.transpileModule(source.slice(start, end), {
  compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020},
  reportDiagnostics: true,
});
assert.equal(compiled.diagnostics.length, 0);
const context = {exports: {}};
vm.runInNewContext(compiled.outputText, context);
const api = context.exports;
const resolve = (preset, base = [], extras = []) =>
  Array.from(api.resolveGraphicsLaunchArguments(preset, base, extras));
const local = (base, current, preset = 'default') =>
  Array.from(api.resolveGraphicsLaunchArguments(preset, base, current, true));
assert.deepEqual(resolve('default', [], ['-video_multisample', '0', '-video_vsync', '0']),
  ['-video_multisample', '0', '-video_vsync', '0']);
assert.deepEqual(local(['-video_app', 'direct3d11'], ['-video_app', 'direct3d9']),
  ['-video_app', 'direct3d9']);
assert.deepEqual(local(['-video_app', 'direct3d11'], []), []);
assert.deepEqual(local(['old'], ['--path', 'Data', '--path', 'data', '', '0', '0']),
  ['--path', 'Data', '--path', 'data', '', '0', '0']);
assert.deepEqual(resolve('default', ['cmd.exe'], ['/c', 'echo', 'echo']),
  ['cmd.exe', '/c', 'echo', 'echo']);
assert.deepEqual(resolve('dx11', [], ['-DX11', '-video_multisample', '0', '-video_vsync', '0', '--', '-dx11']),
  ['-dx11', '-force-d3d11', '-video_multisample', '0', '-video_vsync', '0', '--', '-dx11']);
const preset = api.GRAPHICS_LAUNCH_PRESET_GODOT3_GLES2;
assert.equal(api.normalizeGraphicsLaunchPreset(preset), preset);
assert(api.GRAPHICS_LAUNCH_PRESET_IDS.includes(preset));
assert.equal(api.normalizeGraphicsLaunchPreset('unknown'), 'default');
assert.deepEqual(resolve(preset), ['--video-driver', 'GLES2']);
assert.deepEqual(resolve(preset, ['--video-driver', 'GLES3', '--verbose'],
  ['--video-driver', 'GLES2', '--windowed']),
  ['--video-driver', 'GLES2', '--verbose', '--windowed']);
assert.deepEqual(resolve(preset, ['--video-driver=GLES3']), ['--video-driver', 'GLES2']);
assert.deepEqual(resolve(preset, ['--video-driver', '--verbose']),
  ['--video-driver', 'GLES2', '--verbose']);
// A value belonging to another option must survive the injected pair.
assert.deepEqual(resolve(preset, ['--path', 'GLES2']),
  ['--video-driver', 'GLES2', '--path', 'GLES2']);
assert.deepEqual(resolve(preset, ['--', '--video-driver', 'GLES3']),
  ['--video-driver', 'GLES2', '--', '--video-driver', 'GLES3']);
const base = ['--video-driver', 'GLES3'];
const extras = ['--verbose'];
resolve(preset, base, extras);
assert.deepEqual(base, ['--video-driver', 'GLES3']);
assert.deepEqual(extras, ['--verbose']);
assert.deepEqual(resolve('default'), []);
assert.deepEqual(resolve('default', ['--video-driver', 'GLES3']), base);
assert.deepEqual(resolve('dx11', ['-dx11', '-windowed']),
  ['-dx11', '-force-d3d11', '-windowed']);
assert.deepEqual(resolve('opengl', [], ['-opengl', '-debug']),
  ['-opengl', '-force-glcore', '-debug']);
assert.deepEqual(resolve('unknown', ['-windowed']), ['-windowed']);
console.log('PASS: Godot 3 preset, paired options, existing overrides, defaults and adjacent presets');
