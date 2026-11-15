/* ============================================================================
 * 四足机器狗 步态核心（纯算法，零 DOM 依赖）
 *
 * 机械模型：4 条腿 × 2 个关节 = 8 个舵机，全部在【竖直平面】内动作
 *
 *        髋点 H ──大腿 L1── 膝点 K ──小腿 L2── 脚 F
 *
 *   髋舵机：绕【水平横轴】前后摆 —— 腿在 Y-Z 平面里像钟摆一样前后划
 *   膝舵机：绕【水平横轴】上下折叠 —— 控制腿的伸缩与抬脚
 *
 *   ⚠ 这是狗的腿，不是蜘蛛的腿。
 *   蜘蛛的髋轴是竖直的，腿在水平面里左右划 —— 那套模型算不出抬腿高度，
 *   也无法让脚掌贴地走直线。之前基于蜘蛛模型得出的"左右镜像摆动""净侧漂"
 *   等结论全部作废。
 *
 * 控制策略（比 OpenCat 多两步）：
 *   1. 相位 → 脚的"目标位置"(y, z)，而不是直接给关节角
 *   2. 支撑相：目标 z 恒定（脚贴地、匀速后移）→ 机身不颠
 *   3. 摆动相：目标 z 按 sin² 包络抬起，y 平滑前移 → 不蹭地
 *   4. 两组目标位置经【两连杆逆运动学】解出 8 个舵机角
 *
 *   OpenCat 只驱动肩舵机、膝盖全程固定，靠"斜腿 + 摩擦"把脚蹭过去；
 *   我们的 2-DOF 腿必须真的抬脚、真的落地，所以 IK 是必需的，
 *   它同时承担"摆髋导致脚离地"的补偿 —— 这是机身平稳的关键。
 *
 * 用法：
 *   浏览器  <script src="gait_core.js"></script>  →  window.DogGait
 *   Node    const DogGait = require('./gait_core.js')
 *
 * 坐标约定（单位 mm，右手系）：
 *   +x = 机身右侧      +y = 机身前方      +z = 向上
 *   腿索引固定 0=FL 1=FR 2=RL 3=RR
 * ========================================================================== */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.DogGait = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const DEG = Math.PI / 180;

  /* ---------------------------------------------------------------- 默认参数 */
  const DEFAULTS = {
    periodMs:      1200,  /* 步态周期 ms（MG90S 慢，900ms 以下角速度顶格） */
    dutyPct:       25,    /* 摆动相占周期百分比（波步必须 ≤25%，否则两腿同抬） */
    strideMm:      22,    /* 步幅半幅 A：脚相对髋点前后范围 [-A, +A] */
    liftMm:        16,    /* 摆动相抬脚高度 mm */
    standKneeDeg: -38,    /* 站立时膝角（负 = 小腿向后折，越负机身越低） */
    thighLen:      55,    /* 大腿 L1 mm */
    shinLen:       55,    /* 小腿 L2 mm */
    bodyLen:      120,    /* 机身长 mm（y 方向） */
    bodyWid:       90,    /* 机身宽 mm（x 方向） */
    turn:           0,    /* -100..+100 差速转向：>0 右转 */
    pattern:   'walk',
  };

  /* -------------------------------------------------------------- 腿的几何 */
  /* cx/cy = 髋点在机身坐标系里的归一化位置；side = -1 左 / +1 右 */
  const LEGS = [
    { id: 'FL', side: -1, cx: -0.5, cy: +0.5 },
    { id: 'FR', side: +1, cx: +0.5, cy: +0.5 },
    { id: 'RL', side: -1, cx: -0.5, cy: -0.5 },
    { id: 'RR', side: +1, cx: +0.5, cy: -0.5 },
  ];

  /* ------------------------------------------------------------ 相位偏移表 */
  /* 单位 0~255，256 = 一个完整周期 */
  const PATTERNS = {
    'walk':  { label: '波步（四拍，最稳）', FL:  64, FR: 192, RL:   0, RR: 128 },
    'trot':  { label: '小跑（对角，快）',   FL:   0, FR: 128, RL: 128, RR:   0 },
    'pace':  { label: '溜蹄（同侧）',       FL:   0, FR: 128, RL:   0, RR: 128 },
    'bound': { label: '跳跃（前后成对）',   FL:   0, FR:   0, RL: 128, RR: 128 },
  };

  /* ============================================================ 站立姿态
   * 唯一输入是屈膝角 standKneeDeg，髋角必须由"脚落在髋点正下方"反解：
   *
   *   y = L1·sin(θ1) + L2·sin(θ1+θ2) = 0
   *   → tan(θ1) = -L2·sin(θ2) / (L1 + L2·cos(θ2))
   *
   * 为什么必须这样：如果髋角也随便给，四条腿的脚会整体前后偏移，
   * 机身重心就跑出支撑面 —— 站着都在往前栽。
   * ======================================================================== */
  function standHipOf(par) {
    const t2 = par.standKneeDeg * DEG;
    return Math.atan2(-par.shinLen * Math.sin(t2),
                       par.thighLen + par.shinLen * Math.cos(t2)) / DEG;
  }

  /* 给定任意屈膝角，算机身髋轴离地多高（脚正好落地）。
   * 蹲下 / 趴下 / 站起都靠这个：膝角一改，机身高度必须跟着重算，
   * 否则脚会离地或穿地。 */
  function hipHeightFor(par, kneeDeg) {
    const t2 = kneeDeg * DEG;
    const t1 = Math.atan2(-par.shinLen * Math.sin(t2),
                           par.thighLen + par.shinLen * Math.cos(t2));
    return par.thighLen * Math.cos(t1) + par.shinLen * Math.cos(t1 + t2);
  }

  /* 机身髋轴离地高度：脚刚好落地时髋点有多高 */
  function hipHeightOf(par) {
    return hipHeightFor(par, par.standKneeDeg);
  }

  /* ======================================================= 两连杆逆运动学
   * 输入：脚相对髋点的目标位置 (y 前后, zDown 竖直向下深度)
   * 输出：髋角 hipDeg（0 = 竖直向下，+ = 向前）、膝角 kneeDeg（0 = 伸直，负 = 向后折）
   *
   * 推导：余弦定理先求膝角，再用"总方位角 - 膝的贡献角"求髋角。
   * 膝角强制取负 —— 统一向后折，保证四条腿同构。
   *
   * clamped: 0 = 目标可达；+1 目标太远（腿不够长，被拉直）；-1 目标太近
   * ======================================================================== */
  function ik2(par, y, zDown) {
    const L1 = par.thighLen;
    const L2 = par.shinLen;
    let d = Math.sqrt(y * y + zDown * zDown);
    const dMax = (L1 + L2) * 0.998;
    const dMin = Math.abs(L1 - L2) + 0.5;

    let clamped = 0;
    if (d > dMax) { d = dMax; clamped = 1; }
    if (d < dMin) { d = dMin; clamped = -1; }

    const cosK = (d * d - L1 * L1 - L2 * L2) / (2 * L1 * L2);
    const knee = -Math.acos(Math.max(-1, Math.min(1, cosK)));
    const hip = Math.atan2(y, zDown) -
                Math.atan2(L2 * Math.sin(knee), L1 + L2 * Math.cos(knee));

    return { hipDeg: hip / DEG, kneeDeg: knee / DEG, d: d, clamped: clamped };
  }

  /* ============================================================ 单腿相位解算
   * 输出的是【目标脚位】(y, zLift) 和由它解出的两个关节角。
   *
   *   摆动相 u: 0→1   脚从最后(-A) 平滑移到最前(+A)，同时抬起再落下
   *   支撑相 u: 0→1   脚从最前(+A) 匀速后移到最后(-A)，严格贴地
   *
   * 为什么两组曲线不一样：
   *   支撑相脚必须相对地面静止 → 相对机身匀速后移 → 机身才能匀速前进
   *   摆动相脚要快速越过 → 用余弦缓动，起落瞬间速度为 0，不砸地
   * ======================================================================== */
  function legState(par, leg, phaseNorm) {
    const duty = Math.min(0.9, Math.max(0.05, par.dutyPct / 100));

    let ph = phaseNorm % 1;
    if (ph < 0) ph += 1;

    /* epsilon 容差：相位恰好落在 duty 边界时，浮点误差会让两条腿同时判成摆动。
     * 固件里相位用 0~255 整数，不存在这个问题；仿真里补一个 eps 对齐。 */
    const swinging = ph < duty - 1e-9;
    const u = swinging ? ph / duty : (ph - duty) / (1 - duty);

    /* 转向：左右步幅差动。turn>0 时左侧步幅放大、右侧缩小 → 右转 */
    const k = (par.turn || 0) / 100;
    const scale = leg.side < 0 ? (1 + k * 0.6) : (1 - k * 0.6);
    const stride = par.strideMm * scale;

    let y, zLift;
    if (swinging) {
      const s = (1 - Math.cos(Math.PI * u)) * 0.5;   /* 0→1，两端速度为 0 */
      y = -stride + 2 * stride * s;
      const e = Math.sin(Math.PI * u);                /* sin² 包络：起落速度都为零 */
      zLift = par.liftMm * e * e;
    } else {
      y = stride - 2 * stride * u;                    /* 线性后移 = 匀速前进 */
      zLift = 0;
    }

    const hipH = hipHeightOf(par);
    const ik = ik2(par, y, hipH - zLift);

    return {
      phase: ph, swinging: swinging, u: u,
      targetY: y, targetLift: zLift,
      hipDeg: ik.hipDeg, kneeDeg: ik.kneeDeg,
      reachD: ik.d, clamped: ik.clamped,
    };
  }

  /* ============================================================ 正运动学
   * 由两个关节角还原 8 个关节点的位置（相对机身坐标）。
   * 固件算完角度后，用这个式子反查：脚到底落在哪 —— 也叫"指令校验"。
   *
   *   膝点：y1 = L1·sin(θ1)        z1 = L1·cos(θ1)
   *   脚点：y2 = L2·sin(θ1+θ2)     z2 = L2·cos(θ1+θ2)
   * ======================================================================== */
  function footPos(par, leg, st, hipHOverride) {
    const t1 = st.hipDeg * DEG;
    const t2 = st.kneeDeg * DEG;
    const L1 = par.thighLen;
    const L2 = par.shinLen;

    const y1 = L1 * Math.sin(t1);
    const z1 = L1 * Math.cos(t1);
    const y2 = L2 * Math.sin(t1 + t2);
    const z2 = L2 * Math.cos(t1 + t2);

    /* 蹲下时机身高度不是站立值，允许调用方覆盖 */
    const hipH = (hipHOverride !== undefined && hipHOverride !== null)
      ? hipHOverride : hipHeightOf(par);
    const hipX = leg.cx * par.bodyWid;
    const hipY = leg.cy * par.bodyLen;

    /* 相对髋点的位置（机身坐标，不含机身位移） */
    const offY = y1 + y2;
    const offDown = z1 + z2;

    return {
      hipX: hipX, hipY: hipY, hipZ: hipH,
      kneeX: hipX, kneeY: hipY + y1, kneeZ: hipH - z1,
      x: hipX, y: hipY + offY, z: hipH - offDown,
      offY: offY, offDown: offDown,
      y1: y1, z1: z1, y2: y2, z2: z2,
    };
  }

  /* ============================================================== 状态与推进 */
  function createState() {
    return { t: 0, body: { y: 0, z: 0 }, prevFeet: null };
  }

  /* 解算某一相位下全部四条腿（纯函数） */
  function solve(par, phaseNorm) {
    const table = PATTERNS[par.pattern] || PATTERNS['walk'];
    const legs = [];
    for (let i = 0; i < LEGS.length; i++) {
      const leg = LEGS[i];
      const offset = (table[leg.id] || 0) / 256;
      const st = legState(par, leg, phaseNorm + offset);
      legs.push({ leg: leg, index: i, st: st, foot: footPos(par, leg, st) });
    }
    return legs;
  }

  /* 推进 dtMs 毫秒。SPIDER8 的思路：机身为参照系，支撑脚不动 → 反推机身位移 */
  function step(par, state, dtMs) {
    state.t += dtMs;
    const phaseNorm = (state.t / par.periodMs) % 1;
    const legs = solve(par, phaseNorm);
    const hipH = hipHeightOf(par);

    if (state.prevFeet) {
      let sy = 0, sz = 0, n = 0;
      for (let i = 0; i < legs.length; i++) {
        if (!legs[i].st.swinging) {
          sy += legs[i].foot.offY - state.prevFeet[i].offY;
          sz += legs[i].foot.offDown;
          n++;
        }
      }
      if (n > 0) {
        state.body.y -= sy / n;        /* 支撑脚相对机身后移 → 机身前移 */
        state.body.z = (sz / n) - hipH; /* 支撑脚必须落地 → 机身高度自适应 */
      }
    }

    state.prevFeet = legs.map(function (o) { return o.foot; });
    return { phase: phaseNorm, legs: legs, body: state.body };
  }

  /* MG90S 物理极限：0.1s / 60° → 600°/s。超过这个速度指令角就跑在机械角前面 */
  const SERVO_MAX_RATE = 600;

  /* =========================================================== 周期指标统计
   * 跑满两个周期，取第二个周期（跳过启动瞬态）。
   *
   *   forward    = 每周期前进量 mm
   *   minSupport = 任意时刻最少几条腿同时着地  ← 稳定性，3 条以上才能静态站稳
   *   maxSwing   = 最多几条腿同时抬起
   *   bob        = 机身上下起伏峰峰值 mm       ← 只在腿够不着目标时才 > 0
   *   liftMax    = 一个周期内脚的最大离地高度 mm
   *   maxRate    = 所有关节里最快的角速度 °/s  ← 超过 600 就是舵机跟不上
   *   clampN     = 目标位置不可达的采样次数（腿长不够）
   *
   * 三个指标各自回答一个独立问题，缺一不可：
   *   minSupport → 会不会翻
   *   maxRate    → 舵机会不会拖后腿
   *   bob/clampN → 腿够不够长
   * ======================================================================== */
  function cycleMetrics(par, samples) {
    const n = samples || 720;
    const dt = par.periodMs / n;
    const state = createState();

    step(par, state, 0);
    let base = 0;
    let minZ = Infinity, maxZ = -Infinity;
    let liftMax = 0, clampN = 0;
    let hipMin = Infinity, hipMax = -Infinity;
    let kneeMin = Infinity, kneeMax = -Infinity;
    let minSupport = 4, maxSwing = 0, maxRate = 0;
    let prevAngles = null;

    for (let i = 0; i < n * 2; i++) {
      if (i === n) {
        base = state.body.y;
        minZ = Infinity; maxZ = -Infinity;
      }
      const res = step(par, state, dt);

      /* 支撑腿 / 关节角每条采样都要统计 */
      let sup = 0;
      const angles = [];
      for (let k = 0; k < res.legs.length; k++) {
        const o = res.legs[k];
        if (!o.st.swinging) sup++;
        angles.push({ hip: o.st.hipDeg, knee: o.st.kneeDeg });
      }
      if (sup < minSupport) minSupport = sup;
      if (4 - sup > maxSwing) maxSwing = 4 - sup;

      if (prevAngles) {
        for (let k = 0; k < angles.length; k++) {
          const rh = Math.abs(angles[k].hip - prevAngles[k].hip) / dt * 1000;
          const rk = Math.abs(angles[k].knee - prevAngles[k].knee) / dt * 1000;
          if (rh > maxRate) maxRate = rh;
          if (rk > maxRate) maxRate = rk;
        }
      }
      prevAngles = angles;

      if (i > n) {
        if (state.body.z < minZ) minZ = state.body.z;
        if (state.body.z > maxZ) maxZ = state.body.z;
        for (let k = 0; k < res.legs.length; k++) {
          const o = res.legs[k];
          if (o.foot.z > liftMax) liftMax = o.foot.z;
          if (o.st.clamped) clampN++;
          if (o.st.hipDeg < hipMin) hipMin = o.st.hipDeg;
          if (o.st.hipDeg > hipMax) hipMax = o.st.hipDeg;
          if (o.st.kneeDeg < kneeMin) kneeMin = o.st.kneeDeg;
          if (o.st.kneeDeg > kneeMax) kneeMax = o.st.kneeDeg;
        }
      }
    }

    const dy = state.body.y - base;

    return {
      forward: dy,
      minSupport: minSupport,
      maxSwing: maxSwing,
      bob: (maxZ > minZ && isFinite(maxZ - minZ)) ? (maxZ - minZ) : 0,
      liftMax: liftMax,
      clampN: clampN,
      maxRate: maxRate,
      speed: (dy / par.periodMs) * 1000,
      hipMin: isFinite(hipMin) ? hipMin : 0,
      hipMax: isFinite(hipMax) ? hipMax : 0,
      kneeMin: isFinite(kneeMin) ? kneeMin : 0,
      kneeMax: isFinite(kneeMax) ? kneeMax : 0,
    };
  }

  /* ============================================================== 参数扫描
   * 打分逻辑：先判"能不能用"，再比"跑得快不快"。
   *   不可达 / 超舵机速度     → 直接淘汰（score = -1）
   *   最少支撑腿数 < 3        → 动态步态（trot/bound），降权
   * ======================================================================== */
  function sweep(basePar, axes) {
    const results = [];
    const grid = axes || {};

    const strides  = grid.strideMm  || [18, 24, 30];
    const lifts    = grid.liftMm    || [12, 18, 24];
    const duties   = grid.dutyPct   || [25, 30, 40];
    const patterns = grid.pattern   || Object.keys(PATTERNS);

    patterns.forEach(function (p) {
      duties.forEach(function (d) {
        strides.forEach(function (s) {
          lifts.forEach(function (l) {
            const par = Object.assign({}, DEFAULTS, basePar, {
              pattern: p, dutyPct: d, strideMm: s, liftMm: l,
            });
            const r = cycleMetrics(par, 240);
            const usable = r.clampN === 0 && r.maxRate <= SERVO_MAX_RATE;
            results.push({
              pattern: p, dutyPct: d, strideMm: s, liftMm: l,
              forward: r.forward, bob: r.bob, liftMax: r.liftMax,
              speed: r.speed, clampN: r.clampN,
              minSupport: r.minSupport, maxRate: r.maxRate,
              score: usable ? r.forward * (r.minSupport >= 3 ? 1 : 0.7) / (1 + r.bob * 2) : -1,
            });
          });
        });
      });
    });

    results.sort(function (a, b) { return b.score - a.score; });
    return results;
  }

  return {
    DEFAULTS: DEFAULTS,
    LEGS: LEGS,
    PATTERNS: PATTERNS,
    DEG: DEG,
    SERVO_MAX_RATE: SERVO_MAX_RATE,
    standHipOf: standHipOf,
    hipHeightFor: hipHeightFor,
    hipHeightOf: hipHeightOf,
    ik2: ik2,
    legState: legState,
    footPos: footPos,
    solve: solve,
    createState: createState,
    step: step,
    cycleMetrics: cycleMetrics,
    sweep: sweep,
  };
}));
