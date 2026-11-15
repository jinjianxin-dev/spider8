/* 无头回归测试：stub 掉 DOM/canvas，把 index.html 的内联脚本真跑若干帧。
 *
 * 覆盖三类问题：
 *   A. 画布尺寸每帧翻倍（曾导致"图形一闪而过"）
 *   B. 绘制坐标飞出画布（曾导致"身体往后消失"）
 *   C. 动作切换 / 姿态求解不合法（脚穿地、腿够不着）
 */
const fs = require('fs');
const path = require('path');

const html = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');
const m = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)];
if (m.length !== 1) { console.error('✗ 期望 1 个内联脚本，实际 ' + m.length); process.exit(1); }
const src = m[0][1];

let fails = 0;
function ok(msg) { console.log('✓ ' + msg); }
function bad(msg) { console.error('✗ ' + msg); fails++; }

/* ---------------------------------------------------------- stub: canvas */
/* 记录所有绘制坐标，用来检查是否飞出画布 */
function makeCanvas(id, cssW, cssH) {
  const cv = {
    id, clientWidth: cssW, dataset: { h: String(cssH) },
    _w: 300, _h: 150, resizes: 0,
    pts: [],
  };
  Object.defineProperty(cv, 'width', {
    get() { return cv._w; },
    set(v) { if (cv._w !== v) cv.resizes++; cv._w = v; },
  });
  Object.defineProperty(cv, 'height', {
    get() { return cv._h; },
    set(v) { if (cv._h !== v) cv.resizes++; cv._h = v; },
  });
  cv.listeners = {};
  cv.addEventListener = function (t, fn) { (cv.listeners[t] = cv.listeners[t] || []).push(fn); };
  cv.setPointerCapture = function () {};
  cv.style = {};
  cv.getContext = function () {
    return new Proxy({}, {
      get(t, p) {
        if (p === 'measureText') return function () { return { width: 10 }; };
        if (p === 'moveTo' || p === 'lineTo' || p === 'arc') {
          return function (x, y) { cv.pts.push([x, y]); };
        }
        if (p === 'arcTo') {
          return function (x1, y1, x2, y2) { cv.pts.push([x1, y1], [x2, y2]); };
        }
        return function () {};
      },
      set() { return true; },
    });
  };
  return cv;
}

const canvases = {
  scene: makeCanvas('scene', 780, 460),
  top: makeCanvas('top', 780, 230),
};

/* ------------------------------------------------------ stub: 其他元素 */
const others = {};
function fakeEl(id) {
  if (!others[id]) {
    const el = {
      id, value: '0', textContent: '', _html: '', htmlWrites: 0,
      children: [], listeners: {},
      style: {},
      classList: {
        _on: false,
        toggle(c, v) { this._on = (v === undefined) ? !this._on : !!v; },
        add() {}, remove() {},
      },
      addEventListener(t, fn) { (el.listeners[t] = el.listeners[t] || []).push(fn); },
      appendChild(c) { el.children.push(c); return c; },
      fire(t, ev) { (el.listeners[t] || []).forEach(function (f) { f(ev || {}); }); },
    };
    Object.defineProperty(el, 'innerHTML', {
      get() { return el._html; },
      set(v) { el._html = v; el.htmlWrites++; },
    });
    others[id] = el;
  }
  return others[id];
}

const rafQueue = [];
const windowStub = {
  DogGait: require('./gait_core.js'),
  devicePixelRatio: 2,
  listeners: {},
  addEventListener(t, fn) { (windowStub.listeners[t] = windowStub.listeners[t] || []).push(fn); },
};
const documentStub = {
  getElementById(id) { return canvases[id] || fakeEl(id); },
  createElement() { return fakeEl('_el' + Math.random()); },
};

/* ------------------------------------------------------------- 执行脚本 */
try {
  new Function('window', 'document', 'requestAnimationFrame', src)(
    windowStub, documentStub, function (fn) { rafQueue.push(fn); });
} catch (e) {
  bad('脚本初始化抛异常：' + e.message);
  process.exit(1);
}

/* --------------------------------------------------------------- 跑帧 */
let ts = 1000;
function runFrames(n) {
  for (let i = 0; i < n; i++) {
    ts += 16.7;
    const q = rafQueue.splice(0);
    if (q.length === 0) { bad('第 ' + i + ' 帧 rAF 队列为空，动画停止了'); return false; }
    try {
      q.forEach(function (fn) { fn(ts); });
    } catch (e) {
      bad('第 ' + i + ' 帧抛异常：' + e.message + '\n' + (e.stack || '').split('\n')[1]);
      return false;
    }
  }
  return true;
}

const scene = canvases.scene, top = canvases.top;
const heights = new Set(), widths = new Set();

/* A. 画布尺寸 */
for (let i = 0; i < 180; i++) {
  if (!runFrames(1)) break;
  heights.add(scene._h); widths.add(scene._w);
}
if (heights.size !== 1 || widths.size !== 1) {
  bad('画布物理尺寸在变：height ' + [...heights].join(',') + '  width ' + [...widths].join(','));
} else if (scene._h !== 460 * 2) {
  bad('画布高度 ' + scene._h + '，期望 ' + (460 * 2));
} else {
  ok('180 帧后画布尺寸恒定 ' + scene._w + '×' + scene._h + '（780×460 CSS × dpr2）');
}

/* B. 绘制坐标是否飞出画布 */
function checkBounds(cv, label, cssW, cssH) {
  let oob = 0, nan = 0, minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  cv.pts.forEach(function (p) {
    const x = p[0], y = p[1];
    if (!isFinite(x) || !isFinite(y)) { nan++; return; }
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
    /* 允许小幅溢出（线宽/圆半径），超过 40px 就算跑飞 */
    if (x < -40 || x > cssW + 40 || y < -40 || y > cssH + 40) oob++;
  });
  if (nan > 0) { bad(label + ' 出现 ' + nan + ' 个 NaN 坐标'); return; }
  if (oob > 0) {
    bad(label + ' 有 ' + oob + '/' + cv.pts.length + ' 个坐标飞出画布（范围 x '
        + minX.toFixed(0) + '~' + maxX.toFixed(0) + ', y ' + minY.toFixed(0) + '~' + maxY.toFixed(0) + '）');
    return;
  }
  ok(label + ' 坐标全部在画布内（x ' + minX.toFixed(0) + '~' + maxX.toFixed(0)
     + ', y ' + minY.toFixed(0) + '~' + maxY.toFixed(0) + '）');
}
checkBounds(scene, '3D 视图', 780, 460);
checkBounds(top, '俯视图', 780, 230);

/* B2. 拖到极端视角后仍然不能飞出画布 */
scene.pts.length = 0;
const down = (scene.listeners['pointerdown'] || [])[0];
const moves = windowStub.listeners['pointermove'] || [];
const ups = windowStub.listeners['pointerup'] || [];
if (!down || moves.length === 0) {
  bad('缺少拖拽事件监听（pointerdown/pointermove），无法转视角');
} else {
  /* 极端一：转到背面 + 压到接近水平 */
  down({ clientX: 0, clientY: 0, pointerId: 1, preventDefault: function () {} });
  moves.forEach(function (f) { f({ clientX: -260, clientY: 60 }); });
  ups.forEach(function (f) { f({}); });
  if (!runFrames(20)) { /* 已在 runFrames 内报错 */ }
  checkBounds(scene, '3D 视图（背面 / 低仰角）', 780, 460);

  /* 极端二：转到另一侧 + 抬到接近俯视 */
  scene.pts.length = 0;
  down({ clientX: 0, clientY: 0, pointerId: 1, preventDefault: function () {} });
  moves.forEach(function (f) { f({ clientX: 420, clientY: 260 }); });
  ups.forEach(function (f) { f({}); });
  if (!runFrames(20)) { /* 已在 runFrames 内报错 */ }
  checkBounds(scene, '3D 视图（侧后 / 俯视）', 780, 460);

  /* 回到默认视角，后面的动作测试在正常视角下进行 */
  documentStub.getElementById('camBtn').fire('click');
  runFrames(5);
}

/* C. 动作切换 */
const G = windowStub.DogGait;
let actOk = true;
['stand', 'crouch', 'lie', 'shake', 'wave', 'walk'].forEach(function (name) {
  const btn = others['act-' + name];
  if (!btn) { bad('缺少按钮 act-' + name); actOk = false; return; }
  btn.fire('click');
  if (!runFrames(40)) { actOk = false; return; }
});

if (actOk) {
  /* 直接用核心算法独立校验每个动作姿态的合法性 */
  const par = Object.assign({}, G.DEFAULTS);
  const POSE = [
    ['站立', par.standKneeDeg + 0, null, 0, 0],
    ['蹲下', par.standKneeDeg - 22, null, 0, 0],
    ['趴下', par.standKneeDeg - 57, null, 0, 0],
    ['握手', par.standKneeDeg + 0, 'FR', 22, 34],
    ['挥手', par.standKneeDeg + 0, 'FR', 20, 39],
  ];
  let poseOk = true;
  POSE.forEach(function (p) {
    const knee = p[1];
    const H = G.hipHeightFor(par, knee);
    const dy = p[3], lift = p[4];
    const ik = G.ik2(par, dy, H - lift);
    const servoHip = 90 + (ik.hipDeg - G.standHipOf(par));
    const servoKnee = 90 + (ik.kneeDeg - par.standKneeDeg);
    const foot = G.footPos(par, G.LEGS[2], { hipDeg: ik.hipDeg, kneeDeg: ik.kneeDeg }, H);
    const inRange = servoHip > 5 && servoHip < 175 && servoKnee > 5 && servoKnee < 175;
    const footOk = Math.abs(foot.z - lift) < 0.5;
    if (!inRange || !footOk || ik.clamped) {
      bad('动作「' + p[0] + '」：髋舵机 ' + servoHip.toFixed(1) + '° 膝舵机 '
          + servoKnee.toFixed(1) + '° 脚高 ' + foot.z.toFixed(1) + 'mm'
          + (ik.clamped ? ' 不可达' : ''));
      poseOk = false;
    } else {
      console.log('  ' + p[0] + '：机身 ' + H.toFixed(0) + 'mm，髋 ' + servoHip.toFixed(0)
                  + '° 膝 ' + servoKnee.toFixed(0) + '°，脚高 ' + foot.z.toFixed(0) + 'mm ✓');
    }
  });
  if (poseOk) ok('5 个动作姿态全部可达，舵机角在 0~180° 内');
}

console.log('');
console.log(fails === 0 ? '全部通过' : '有 ' + fails + ' 项失败');
process.exit(fails === 0 ? 0 : 1);
