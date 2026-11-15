/* ============================================================================
 * 步态核心数值验证 —— 不碰硬件，先把几何和参数算死
 *
 * 检查项：
 *   Q1 站立姿态：脚是否刚好落地
 *   Q2 IK 精度：目标位置 vs 正运动学还原的位置
 *   Q3 单腿轨迹：一个周期内脚怎么走，抬多高、往前迈多少
 *   Q4 周期指标：前进量、机身起伏、可达性
 *   Q5 舵机行程：8 个舵机是否都在 MG90S 的 0~180° 内
 *   Q6 步态对比：walk / trot / pace / bound
 *   Q7 参数扫描：步幅和抬腿高度怎么选
 * ========================================================================== */
const G = require('./gait_core.js');
const D = G.DEG;
const f = (v, n) => v.toFixed(n === undefined ? 1 : n);
const pad = (s, n) => String(s).padStart(n);

function line() { console.log(''); }

/* ---------------------------------------------------------- Q1 站立姿态 */
{
  const par = Object.assign({}, G.DEFAULTS);
  const H = G.hipHeightOf(par);
  const hip = G.standHipOf(par);
  console.log('=== Q1 站立姿态 ===');
  console.log('大腿 ' + par.thighLen + 'mm + 小腿 ' + par.shinLen + 'mm = 全伸 ' +
              (par.thighLen + par.shinLen) + 'mm');
  console.log('屈膝角 ' + par.standKneeDeg + '° → 髋角自动反解 ' + f(hip) + '°（脚落髋点正下方）');
  console.log('→ 机身髋轴离地高 ' + f(H) + 'mm（比全伸矮 ' +
              f(par.thighLen + par.shinLen - H) + 'mm，这就是屈膝量）');
  const st = { hipDeg: hip, kneeDeg: par.standKneeDeg };
  const fp = G.footPos(par, G.LEGS[0], st);
  console.log('站立位（脚在正下方）：脚高 z = ' + f(fp.z) + 'mm，脚前后偏移 y = ' + f(fp.offY) + 'mm  ' +
              (Math.abs(fp.z) < 0.01 && Math.abs(fp.offY) < 0.01 ? '✓ 贴地且在正下方' : '✗'));
  console.log('膝舵机实际角（映射 90°=伸直）：' + f(90 + par.standKneeDeg) + '°');
}

/* --------------------------------------------------------------- Q2 IK 精度 */
{
  console.log('');
  console.log('=== Q2 IK 精度（给定目标 → 解角度 → 正运动学还原）===');
  const par = Object.assign({}, G.DEFAULTS);
  const H = G.hipHeightOf(par);
  console.log('  目标y   目标zD    髋角     膝角   还原y   还原zD  误差');
  let worst = 0;
  [[0, H], [24, H], [-24, H], [24, H - 18], [0, H - 18], [-12, H - 9]].forEach(function (t) {
    const ik = G.ik2(par, t[0], t[1]);
    const st = { hipDeg: ik.hipDeg, kneeDeg: ik.kneeDeg };
    const fp = G.footPos(par, G.LEGS[0], st);
    const err = Math.hypot(fp.offY - t[0], fp.offDown - t[1]);
    if (err > worst) worst = err;
    console.log('  ' + pad(t[0], 6) + '  ' + pad(f(t[1]), 7) + '  ' +
                pad(f(ik.hipDeg), 7) + '  ' + pad(f(ik.kneeDeg), 7) + '  ' +
                pad(f(fp.offY), 7) + '  ' + pad(f(fp.offDown), 7) + '  ' +
                f(err, 3) + (err < 0.01 ? ' ✓' : ' ✗'));
  });
  console.log('最大误差 ' + f(worst, 4) + 'mm ' + (worst < 0.01 ? '✓ IK 正确' : '✗ 推导有误'));
}

/* ------------------------------------------------------------ Q3 单腿轨迹 */
{
  console.log('');
  console.log('=== Q3 单腿轨迹（FL，一个周期，duty=' + G.DEFAULTS.dutyPct + '%）===');
  const par = Object.assign({}, G.DEFAULTS);
  console.log('  相位   相态   目标y   抬脚   髋角     膝角    脚z     脚y');
  for (let i = 0; i <= 10; i++) {
    const ph = i / 10;
    const st = G.legState(par, G.LEGS[0], ph);
    const fp = G.footPos(par, G.LEGS[0], st);
    console.log('  ' + f(ph, 2) + '  ' + (st.swinging ? '摆动' : '支撑') + '  ' +
                pad(f(st.targetY), 6) + '  ' + pad(f(st.targetLift), 5) + '  ' +
                pad(f(st.hipDeg), 7) + '  ' + pad(f(st.kneeDeg), 7) + '  ' +
                pad(f(fp.z), 6) + '  ' + pad(f(fp.offY), 6) +
                (st.clamped ? '  ⚠不可达' : ''));
  }
}

/* ------------------------------------------------------------ Q4 周期指标 */
{
  console.log('');
  console.log('=== Q4 周期指标（默认参数 stride=' + G.DEFAULTS.strideMm +
              'mm period=' + G.DEFAULTS.periodMs + 'ms）===');
  console.log('步态   每周期  最少支撑  同时抬  起伏  抬脚   速度   峰值角速  舵机  不可达');
  Object.keys(G.PATTERNS).forEach(function (p) {
    const par = Object.assign({}, G.DEFAULTS, { pattern: p });
    const m = G.cycleMetrics(par, 720);
    const rateOk = m.maxRate <= G.SERVO_MAX_RATE;
    console.log(pad(p, 6) + pad(f(m.forward) + 'mm', 8) +
                pad(m.minSupport + '条', 10) + pad(m.maxSwing + '条', 7) +
                pad(f(m.bob) + 'mm', 7) + pad(f(m.liftMax) + 'mm', 7) +
                pad(f(m.speed), 7) + pad(f(m.maxRate) + '°/s', 10) +
                pad(rateOk ? '✓' : '✗超速', 6) +
                pad(m.clampN, 6) + (m.clampN ? ' ⚠' : ' ✓'));
  });
  console.log('');
  console.log('注：最少支撑 ≥ 3 条 = 静态稳定（抬一条腿重心还在三角支撑面内）');
  console.log('    最少支撑 = 2 条 = 动态平衡，必须靠惯性往前冲才不会倒');
  console.log('    峰值角速上限 ' + G.SERVO_MAX_RATE + '°/s（MG90S 0.1s/60°）');
}

/* ------------------------------------------------------------ Q5 舵机行程 */
{
  console.log('');
  console.log('=== Q5 舵机行程校核 ===');
  console.log('映射约定：舵机 90° = 站立姿态（标定时全部打 90° 装舵机臂）');
  const par = Object.assign({}, G.DEFAULTS);
  const m = G.cycleMetrics(par, 720);
  const hipStand = G.standHipOf(par);
  const kneeStand = par.standKneeDeg;

  const hipLo = 90 + (m.hipMin - hipStand);
  const hipHi = 90 + (m.hipMax - hipStand);
  const kneeLo = 90 + (m.kneeMin - kneeStand);
  const kneeHi = 90 + (m.kneeMax - kneeStand);

  const rows = [
    ['髋/肩舵机', hipLo, hipHi, 90],
    ['膝舵机',    kneeLo, kneeHi, 90],
  ];
  let ok = true;
  console.log('通道       最低角    最高角    站立角   跨度');
  rows.forEach(function (r) {
    const inRange = r[1] >= 5 && r[2] <= 175;   /* 两端各留 5° 死区余量 */
    if (!inRange) ok = false;
    console.log(pad(r[0], 10) + pad(f(r[1]), 8) + '°' + pad(f(r[2]), 8) + '°' +
                pad(r[3], 8) + '°' + pad(f(r[2] - r[1]), 7) + '°   ' +
                (inRange ? '✓' : '✗ 超行程'));
  });
  console.log('');
  console.log('站立时 8 个舵机全部是 90°；行走时只在上面这个区间里摆动。');
  console.log(ok ? '✓ 全部落在 MG90S 的 0~180° 内（两端各留 5° 余量）'
                : '✗ 行程不够，需要改安装中位或减小步幅/抬腿');
}

/* ------------------------------------------------------------ Q6 步态对比 */
{
  console.log('');
  console.log('=== Q6 步态特性对比 ===');
  console.log('波步 walk   : 四腿依次抬，任意时刻仅 1 腿离地 → 最稳，静态平衡');
  console.log('小跑 trot   : 对角两腿成对，2 腿同时离地 → 快，但需要动态平衡');
  console.log('溜蹄 pace   : 同侧两腿成对 → 机身侧向摇摆大，不推荐');
  console.log('跳跃 bound  : 前后成对 → 弹跳式前进，需要较大抬脚');
}

/* ------------------------------------------------------------ Q7 参数扫描 */
{
  console.log('');
  console.log('=== Q7 参数扫描（walk / trot，按 score 排序）===');
  const res = G.sweep({}, {
    pattern: ['walk', 'trot'],
    dutyPct: [25, 30, 40],
    strideMm: [18, 24, 30],
    liftMm: [12, 18, 24],
  });
  console.log('步态   占空比  步幅   抬脚   每周期   最少支撑  峰值角速   速度    判定');
  res.slice(0, 14).forEach(function (r) {
    const ok = r.score >= 0;
    console.log(pad(r.pattern, 6) + pad(r.dutyPct + '%', 8) + pad(r.strideMm + 'mm', 7) +
                pad(r.liftMm + 'mm', 7) + pad(f(r.forward) + 'mm', 8) +
                pad(r.minSupport + '条', 9) + pad(f(r.maxRate) + '°/s', 10) +
                pad(f(r.speed), 8) + '   ' + (ok ? (r.minSupport >= 3 ? '✓稳' : '△动态') : '✗淘汰'));
  });
  const dead = res.filter(function (r) { return r.score < 0; }).length;
  console.log('');
  console.log('共 ' + res.length + ' 组，淘汰 ' + dead + ' 组（不可达或超舵机速度）');
}

/* -------------------------------------------------- Q8 机身起伏根因对照 */
{
  console.log('');
  console.log('=== Q8 为什么要 IK：只摆髋不做膝补偿会怎样 ===');
  const par = Object.assign({}, G.DEFAULTS);
  const H = G.hipHeightOf(par);
  const m = G.cycleMetrics(par, 720);
  const hipMid = (m.hipMin + m.hipMax) / 2;
  const hipAmp = (m.hipMax - m.hipMin) / 2;

  /* 反例：髋角直接按余弦摆，膝角锁死在站立值 —— 这正是 OpenCat 的做法 */
  let minZ = Infinity, maxZ = -Infinity;
  for (let i = 0; i <= 400; i++) {
    const ph = i / 400;
    const ang = hipMid + hipAmp * Math.cos(2 * Math.PI * ph);
    const fp = G.footPos(par, G.LEGS[0], { hipDeg: ang, kneeDeg: par.standKneeDeg });
    if (fp.z < minZ) minZ = fp.z;
    if (fp.z > maxZ) maxZ = fp.z;
  }
  console.log('髋角按余弦摆 ' + f(hipMid - hipAmp) + '~' + f(hipMid + hipAmp) +
              '°，膝角锁死 ' + par.standKneeDeg + '°：');
  console.log('  → 脚离地高度在 ' + f(minZ) + ' ~ ' + f(maxZ) + 'mm 之间漂，' +
              '峰峰 ' + f(maxZ - minZ) + 'mm');
  console.log('  → 这就是机身上下颠的来源，OpenCat 靠"斜腿贴地 + 摩擦"糊过去');
  console.log('');
  console.log('我们加 IK 让脚严格贴地后：机身起伏 ' + f(m.bob, 3) + 'mm ' +
              (m.bob < 0.5 ? '✓ 颠簸被消掉' : '（有腿够不着目标）'));
  console.log('代价：膝角要多摆动 ' + f(m.kneeMax - m.kneeMin) + '°，这 8 个舵机必须跟得上');
}

line();
console.log('验证结束。');
