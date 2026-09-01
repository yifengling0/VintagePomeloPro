#!/usr/bin/env node

// 虚拟输入控件 v6 模型单测。
// 用法：node scripts/run_input_controls_unit_tests.cjs <compiled-model-dir>
// compiled-model-dir 需包含 model/AppModels.js 与 common/EvdevKeyNames.js
// （由 tsc 编译 entry/src/main/ets 中的对应 .ets 得到）。

const assert = require('node:assert/strict');
const path = require('node:path');

const outputDirectory = process.argv[2];
if (!outputDirectory) {
  throw new Error('compiled model directory is required');
}

const models = require(path.resolve(outputDirectory, 'model/AppModels.js'));
const keynames = require(path.resolve(outputDirectory, 'common/EvdevKeyNames.js'));

// ---- 模板工厂 ----
assert.equal(models.INPUT_TEMPLATES.length, 5);
assert.equal(models.INPUT_PROFILE_SCHEMA_VERSION, 6);
assert.deepEqual(models.INPUT_TEMPLATES.map((template) => template.id),
  ['default', 'full-keyboard', 'rpg', 'fps', 'action']);

const builtIns = models.createBuiltInInputProfiles();
assert.deepEqual(builtIns.map((profile) => profile.id),
  ['default', 'builtin-full-keyboard', 'builtin-rpg', 'builtin-fps', 'builtin-action']);
assert.ok(builtIns.every((profile) => models.isBuiltInInputProfileId(profile.id)));
assert.equal(models.isBuiltInInputProfileId('desktop-user'), false);
assert.deepEqual(builtIns.map((profile) => profile.builtInRevision), [4, 14, 5, 3, 3]);

const generic = models.createDefaultInputProfile();
assert.equal(generic.id, 'default');
assert.equal(generic.schemaVersion, 6);
assert.ok(generic.elements.length >= 8, '默认模板应包含至少 8 个元素');
assert.ok(generic.gamepadMappings.length >= 10, '默认手柄映射应保留');

const keyboard = models.createInputProfileFromTemplate('keyboard', '全键盘', 'full-keyboard');
assert.ok(keyboard.elements.length >= 70, '全键盘模板应覆盖主要 PC 键区');
assert.equal(keyboard.elements.some((e) => e.type === 'PANEL'), false,
  '当前全键盘模板使用独立按键，不恢复已经移除的外框');
assert.ok(keyboard.elements.some((e) => e.text === 'F12' && e.bindings[0].code === 88));
assert.ok(keyboard.elements.some((e) => e.text === 'Space' && e.bindings[0].code === 57));
assert.ok(keyboard.elements.some((e) => e.text === '↑' && e.bindings[0].code === 103));
assert.ok(keyboard.elements.every((e) => e.keyWidthRatio > 0 && e.keyHeightRatio > 0),
  '全键盘应声明按视口缩放的键帽尺寸');
assert.ok(keyboard.elements.some((e) => e.text === 'Home' && e.x > 0.8),
  '导航键区应与主键区分离');
assert.ok(keyboard.elements.some((e) => e.text === 'A' && e.x >
  keyboard.elements.find((key) => key.text === 'Caps').x), '主键区应按物理键盘行列排序');

const rpg = models.createInputProfileFromTemplate('rpg', 'RPG 方案', 'rpg');
assert.ok(rpg.elements.some((e) => e.text === '背包' && e.bindings[0].code === 23));
assert.ok(rpg.elements.some((e) => e.text === '地图' && e.bindings[0].code === 50));
assert.ok(rpg.elements.filter((e) => ['菜单', '背包', '地图', '角色', '任务'].includes(e.text))
  .every((e) => e.y <= 0.1), 'RPG 菜单快捷键应贴近顶部');
assert.ok(rpg.elements.some((e) => e.type === 'TRACKPAD' && e.y >= 0.55),
  'RPG 鼠标触摸区应放在画面下半区');
assert.ok(rpg.elements.some((e) => e.type === 'RANGE_BUTTON' && e.orientation === 'vertical' && e.x >= 0.95),
  'RPG 技能快捷键应为右侧细竖条');

const shooter = models.createInputProfileFromTemplate('p1', '射击方案', 'fps');
assert.equal(shooter.name, '射击方案');
assert.equal(shooter.elements.length, 10);
assert.equal(shooter.elements[0].type, 'D_PAD');
assert.equal(shooter.elements[0].bindings.length, 4);
assert.equal(shooter.elements[0].bindings[0].kind, 'KEY');
assert.equal(shooter.elements[0].bindings[0].code, 17); // W
assert.equal(shooter.elements[1].type, 'TRACKPAD');
assert.ok(shooter.elements.some((e) => e.type === 'RANGE_BUTTON' && e.keyList.length >= 4));

const action = models.createInputProfileFromTemplate('p2', '动作方案', 'action');
assert.equal(action.elements[0].type, 'STICK');
assert.equal(action.elements[1].type, 'STICK');
assert.equal(action.elements[1].bindings[0].code, 103); // 上

// ---- 手机/平板默认模板几何：修饰键不能压住移动摇杆或方向键 ----
function viewportScale(width, height) {
  const longSide = Math.max(width, height);
  const shortSide = Math.min(width, height);
  return Math.max(0.85, Math.min(1.65, Math.min(longSide / 809, shortSide / 365)));
}

function controlSize(element, width, height) {
  if (element.type === 'PANEL') {
    return [width * (element.panelWidthRatio ?? 0.84), height * (element.panelHeightRatio ?? 0.48)];
  }
  if (element.type === 'BUTTON' && element.keyWidthRatio > 0 && element.keyHeightRatio > 0) {
    return [width * element.keyWidthRatio * element.scale * (element.widthScale ?? 1),
      height * element.keyHeightRatio * element.scale * (element.heightScale ?? 1)];
  }
  const responsive = viewportScale(width, height);
  if (element.type === 'TRACKPAD') {
    return [150 * element.scale * (element.widthScale ?? 1) * responsive,
      100 * element.scale * (element.heightScale ?? 1) * responsive];
  }
  if (element.type === 'RANGE_BUTTON') {
    const horizontal = element.orientation === 'horizontal';
    return [(horizontal ? 180 : 44) * element.scale * (element.widthScale ?? 1) * responsive,
      (horizontal ? 44 : 180) * element.scale * (element.heightScale ?? 1) * responsive];
  }
  const base = element.type === 'D_PAD' ? 132 : element.type === 'STICK' ? 120 : 56;
  return [base * element.scale * (element.widthScale ?? 1) * responsive,
    base * element.scale * (element.heightScale ?? 1) * responsive];
}

function overlaps(left, right, width, height) {
  const [leftW, leftH] = controlSize(left, width, height);
  const [rightW, rightH] = controlSize(right, width, height);
  return Math.abs(left.x - right.x) * width < (leftW + rightW) / 2 &&
    Math.abs(left.y - right.y) * height < (leftH + rightH) / 2;
}

for (const profile of [generic, rpg, shooter, action]) {
  const movement = profile.elements.find((e) => e.type === 'D_PAD' || e.type === 'STICK');
  const modifiers = profile.elements.filter((e) => e.text === 'Shift' || e.text === 'Ctrl');
  assert.ok(modifiers.length > 0 && modifiers.every((modifier) => modifier.toggleSwitch),
    `${profile.name} 的 Shift/Ctrl 应默认启用按住锁定`);
  assert.ok(modifiers.every((modifier) => modifier.y < movement.y),
    `${profile.name} 的 Shift/Ctrl 应放在移动区上方`);
  for (const [width, height] of [[809, 365], [1280, 800]]) {
    assert.ok(modifiers.every((modifier) => !overlaps(movement, modifier, width, height)),
      `${profile.name} 的 Shift/Ctrl 不应在 ${width}x${height} 下遮挡移动控件`);
    for (const modifier of modifiers) {
      const others = profile.elements.filter((element) =>
        element !== modifier && element.type !== 'PANEL');
      assert.ok(others.every((element) => !overlaps(modifier, element, width, height)),
        `${profile.name} 的 ${modifier.text} 不应在 ${width}x${height} 下遮挡其他控件`);
    }
  }
}

for (const [width, height] of [[809, 365], [1280, 800]]) {
  const rpgControls = rpg.elements.filter((element) => element.type !== 'PANEL');
  for (let left = 0; left < rpgControls.length; left++) {
    for (let right = left + 1; right < rpgControls.length; right++) {
      assert.ok(!overlaps(rpgControls[left], rpgControls[right], width, height),
        `RPG 控件 ${rpgControls[left].text}/${rpgControls[right].text} 不应在 ${width}x${height} 下重叠`);
    }
  }
}

const keyboardModifiers = keyboard.elements.filter((e) => e.text === 'Shift' || e.text === 'Ctrl');
assert.ok(keyboardModifiers.length >= 4 && keyboardModifiers.every((modifier) => modifier.toggleSwitch),
  '全键盘左右 Shift/Ctrl 应默认启用按住锁定');
for (const [width, height] of [[809, 365], [1280, 800]]) {
  for (const key of keyboard.elements.filter((element) => element.type === 'BUTTON')) {
    const [keyW, keyH] = controlSize(key, width, height);
    assert.ok(key.x * width - keyW / 2 >= 0 && key.x * width + keyW / 2 <= width &&
      key.y * height - keyH / 2 >= 0 && key.y * height + keyH / 2 <= height,
    `全键盘按键 ${key.text} 不应在 ${width}x${height} 下越过视口`);
  }
}

// ---- 克隆与序列化往返 ----
const cloned = models.cloneInputProfile(generic);
assert.equal(JSON.stringify(cloned), JSON.stringify(generic));
const roundtrip = models.cloneInputProfile(JSON.parse(JSON.stringify(generic)));
assert.equal(roundtrip.elements.length, generic.elements.length);
assert.equal(roundtrip.overlayOpacity, generic.overlayOpacity);

// ---- 旧三套结构兼容：缺失 elements 时回退读取平板套 ----
const legacyProfile = JSON.parse(JSON.stringify(generic));
delete legacyProfile.elements;
legacyProfile.elementsTablet = JSON.parse(JSON.stringify(generic.elements));
const migrated = models.cloneInputProfile(legacyProfile);
assert.equal(migrated.elements.length, generic.elements.length);

// ---- 真实 Winlator GTA5 .icp 片段导入 ----
const gtaFragment = JSON.stringify({
  id: 16,
  name: 'GTA 5',
  cursorSpeed: 1,
  elements: [
    {
      type: 'BUTTON',
      shape: 'CIRCLE',
      bindings: ['MOUSE_LEFT_BUTTON', 'NONE', 'NONE', 'NONE'],
      scale: 1,
      x: 0.813,
      y: 0.733,
      toggleSwitch: false,
      text: '',
      iconId: 0
    },
    {
      type: 'BUTTON',
      shape: 'RECT',
      bindings: ['KEY_SHIFT_L', 'NONE', 'NONE', 'NONE'],
      scale: 1,
      x: 0.078,
      y: 0.088,
      toggleSwitch: true,
      text: '',
      iconId: 0
    },
    {
      type: 'STICK',
      shape: 'CIRCLE',
      bindings: ['KEY_W', 'KEY_D', 'KEY_S', 'KEY_A'],
      scale: 1,
      x: 0.107,
      y: 0.733,
      toggleSwitch: false,
      text: '',
      iconId: 0
    },
    {
      type: 'UNKNOWN_TYPE',
      shape: 'CIRCLE',
      bindings: ['KEY_Q'],
      scale: 1,
      x: 0.5,
      y: 0.5
    }
  ]
});

const imported = models.importProfileFromIcpJson(gtaFragment, 'desktop-test');
assert.equal(imported.schemaVersion, 6);
assert.equal(imported.name, 'GTA 5');
assert.equal(imported.elements.length, 3, '未知元素类型应跳过');
assert.equal(imported.elements[0].type, 'BUTTON');
assert.equal(imported.elements[0].bindings[0].kind, 'MOUSE');
assert.equal(imported.elements[0].bindings[0].code, 272);
assert.equal(imported.elements[0].shape, 'CIRCLE');
assert.equal(imported.elements[1].toggleSwitch, true);
assert.equal(imported.elements[1].bindings[0].code, 42); // Shift
assert.equal(imported.elements[2].type, 'STICK');
assert.deepEqual(imported.elements[2].bindings.map((b) => b.code), [17, 32, 31, 30]);

// ---- 导出为 .icp 兼容子集 ----
const exportedJson = models.exportProfileToIcpJson(shooter);
const exported = JSON.parse(exportedJson);
assert.equal(exported.name, '射击方案');
assert.equal(exported.cursorSpeed, 1);
assert.equal(exported.elements.length, shooter.elements.length);
assert.ok(exported.elements.every((e) =>
  e.bindings !== undefined && e.bindings.length === 4));
assert.equal(exported.elements[0].bindings[0], 'KEY_W');

const reimported = models.importProfileFromIcpJson(exportedJson, 'desktop-roundtrip');
assert.equal(reimported.elements.length, shooter.elements.length);
assert.equal(reimported.elements[0].type, 'D_PAD');
assert.deepEqual(reimported.elements[0].bindings.map((b) => b.code), [17, 32, 31, 30]);

// ---- 绑定名映射表 ----
assert.equal(keynames.evdevNameToBinding('KEY_W').code, 17);
assert.equal(keynames.evdevNameToBinding('KEY_W').kind, 'KEY');
assert.equal(keynames.evdevNameToBinding('MOUSE_LEFT_BUTTON').code, 272);
assert.equal(keynames.evdevNameToBinding('MOUSE_LEFT_BUTTON').kind, 'MOUSE');
assert.equal(keynames.evdevNameToBinding('KEY_SHIFT_L').code, 42);
assert.equal(keynames.evdevNameToBinding('KEY_ESCAPE').code, 1); // 别名
assert.equal(keynames.evdevNameToBinding('KEY_LEFTCTRL').code, 29); // 别名
assert.equal(keynames.evdevNameToBinding('NONE').kind, 'NONE');
assert.equal(keynames.evdevNameToBinding('KEY_NOT_EXIST').kind, 'NONE');
assert.equal(keynames.evdevNameToBinding('').kind, 'NONE');
assert.equal(keynames.bindingToEvdevName('KEY', 17), 'KEY_W');
assert.equal(keynames.bindingToEvdevName('MOUSE', 273), 'MOUSE_RIGHT_BUTTON');
assert.equal(keynames.bindingToEvdevName('KEY', 0), 'NONE');

// 全量映射表可解析：每个导出名都能导入回同一码
for (const option of keynames.KEY_NAME_OPTIONS) {
  const parsed = keynames.evdevNameToBinding(option.name);
  assert.equal(parsed.kind, 'KEY', option.name);
  assert.equal(parsed.code, option.code, option.name);
}

console.log('input controls v6 unit tests passed');
