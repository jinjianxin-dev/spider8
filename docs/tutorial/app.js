/* ==========================================================================
   Spider8 开发手册 · 交互逻辑
   无外部依赖，纯原生 JS。所有状态存 localStorage（localStorage 不可用时静默降级）
   ========================================================================== */
(function () {
  'use strict';

  var CHAPTERS = window.CHAPTERS || [];
  var KEY = { ck: 'spider8.tut.ck.', done: 'spider8.tut.chap.', theme: 'spider8.tut.theme' };

  /* ------------------------------------------------------------ 工具 */
  var store = {
    get: function (k, d) {
      try {
        var v = localStorage.getItem(k);
        return v === null ? d : v;
      } catch (e) { return d; }
    },
    set: function (k, v) {
      try { localStorage.setItem(k, v); } catch (e) { /* 隐私模式下忽略 */ }
    }
  };

  var toastEl = document.getElementById('toast');
  var toastTimer = null;
  function toast(msg) {
    if (!toastEl) return;
    toastEl.textContent = msg;
    toastEl.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { toastEl.classList.remove('show'); }, 1600);
  }

  function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  /* 极简语法着色：只处理字符串与注释，避免误伤代码 */
  function highlight(text, lang) {
    var html = escapeHtml(text);
    var re;
    if (lang === 'powershell') {
      re = /("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|#.*$)/gm;
    } else if (lang === 'c' || lang === 'cpp') {
      re = /("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|\/\/.*$|\/\*[\s\S]*?\*\/)/gm;
    } else if (lang === 'json') {
      re = /("(?:[^"\\]|\\.)*")/g;
    } else {
      return html;
    }
    return html.replace(re, function (m) {
      var two = m.slice(0, 2);
      if (m.charAt(0) === '#' || two === '//' || two === '/*') {
        return '<span class="cm">' + m + '</span>';
      }
      return '<span class="st">' + m + '</span>';
    });
  }

  function copyText(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      return navigator.clipboard.writeText(text);
    }
    return new Promise(function (resolve, reject) {
      try {
        var ta = document.createElement('textarea');
        ta.value = text;
        ta.style.position = 'fixed';
        ta.style.top = '-9999px';
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
        resolve();
      } catch (e) { reject(e); }
    });
  }

  /* ------------------------------------------------------------ 渲染正文 */
  var main = document.getElementById('main');
  if (!CHAPTERS.length) { return; }

  var frag = document.createDocumentFragment();

  CHAPTERS.forEach(function (ch) {
    var sec = document.createElement('section');
    sec.className = 'chapter';
    sec.id = ch.id;
    sec.innerHTML =
      '<div class="ch-head">' +
        '<span class="ch-num">' + ch.num + '</span>' +
        '<div><h1>' + ch.title + '</h1><p class="ch-sub">' + ch.subtitle + '</p></div>' +
      '</div>' +
      ch.html +
      '<div class="ch-foot">' +
        '<button class="btn-done" data-ch="' + ch.id + '">标记本节已完成</button>' +
        '<span class="hint">进度保存在本机浏览器</span>' +
      '</div>';
    frag.appendChild(sec);
  });

  main.appendChild(frag);

  var sections = Array.prototype.slice.call(main.querySelectorAll('.chapter'));

  /* 给每个 h2 生成锚点 id */
  sections.forEach(function (sec) {
    var h2s = sec.querySelectorAll('h2');
    Array.prototype.forEach.call(h2s, function (h2, i) {
      h2.id = sec.id + '-' + (i + 1);
    });
  });

  /* ------------------------------------------------------------ 目录 */
  var nav = document.getElementById('nav');

  sections.forEach(function (sec) {
    var ch = null;
    for (var i = 0; i < CHAPTERS.length; i++) {
      if (CHAPTERS[i].id === sec.id) { ch = CHAPTERS[i]; break; }
    }

    var li = document.createElement('li');
    li.setAttribute('data-for', sec.id);

    var a = document.createElement('a');
    a.href = '#' + sec.id;
    a.innerHTML = '<span class="num">' + (ch ? ch.num : '') + '</span>' +
                  '<span>' + (ch ? ch.title : '') + '</span>' +
                  '<span class="done-mark">✓</span>';
    li.appendChild(a);

    var sub = document.createElement('ul');
    sub.className = 'subnav';
    Array.prototype.forEach.call(sec.querySelectorAll('h2'), function (h2) {
      var sli = document.createElement('li');
      var sa = document.createElement('a');
      sa.href = '#' + h2.id;
      sa.textContent = h2.textContent;
      sli.appendChild(sa);
      sub.appendChild(sli);
    });
    li.appendChild(sub);

    nav.appendChild(li);
  });

  /* ------------------------------------------------------------ 代码块装饰 */
  Array.prototype.forEach.call(main.querySelectorAll('.code'), function (box) {
    var lang = (box.getAttribute('data-lang') || 'code').toLowerCase();
    var pre = box.querySelector('pre');
    if (!pre) { return; }
    var codeEl = pre.querySelector('code');

    if (codeEl) {
      var raw = codeEl.textContent;
      codeEl.innerHTML = highlight(raw, lang);
    }

    var bar = document.createElement('div');
    bar.className = 'code-bar';

    var label = document.createElement('span');
    label.className = 'lang';
    label.textContent = lang === 'code' ? 'code' : lang;

    var btn = document.createElement('button');
    btn.className = 'copy-btn';
    btn.type = 'button';
    btn.textContent = '复制';
    btn.addEventListener('click', function () {
      copyText(pre.innerText).then(function () {
        btn.textContent = '已复制';
        toast('已复制到剪贴板');
        setTimeout(function () { btn.textContent = '复制'; }, 1500);
      }, function () {
        toast('复制失败，请手动选择');
      });
    });

    bar.appendChild(label);
    bar.appendChild(btn);
    box.insertBefore(bar, pre);
  });

  /* ------------------------------------------------------------ 进度 */
  var doneCountEl = document.getElementById('doneCount');
  var totalCountEl = document.getElementById('totalCount');
  var doneBar = document.getElementById('doneBar');

  function isChapDone(id) { return store.get(KEY.done + id, '0') === '1'; }

  function refreshProgress() {
    var done = 0;
    sections.forEach(function (sec) {
      var btn = sec.querySelector('.btn-done');
      var link = nav.querySelector('li[data-for="' + sec.id + '"] > a');
      if (isChapDone(sec.id)) {
        done++;
        if (btn) { btn.classList.add('done'); btn.textContent = '已完成 ✓'; }
        if (link) { link.classList.add('is-done'); }
      } else {
        if (btn) { btn.classList.remove('done'); btn.textContent = '标记本节已完成'; }
        if (link) { link.classList.remove('is-done'); }
      }
    });

    if (doneCountEl) { doneCountEl.textContent = done; }
    if (totalCountEl) { totalCountEl.textContent = sections.length; }
    if (doneBar) { doneBar.style.width = (sections.length ? (done / sections.length) * 100 : 0) + '%'; }
  }

  main.addEventListener('click', function (e) {
    var btn = e.target.closest ? e.target.closest('.btn-done') : null;
    if (!btn) { return; }
    var id = btn.getAttribute('data-ch');
    var next = isChapDone(id) ? '0' : '1';
    store.set(KEY.done + id, next);
    refreshProgress();
    toast(next === '1' ? '已标记完成' : '已取消标记');
  });

  /* ------------------------------------------------------------ 验收清单 */
  Array.prototype.forEach.call(main.querySelectorAll('.checklist input[type="checkbox"]'), function (cb) {
    var key = KEY.ck + cb.getAttribute('data-ck');
    if (store.get(key, '0') === '1') { cb.checked = true; }
    cb.addEventListener('change', function () {
      store.set(key, cb.checked ? '1' : '0');
    });
  });

  /* ------------------------------------------------------------ 故障搜索 */
  var faultSearch = document.getElementById('faultSearch');
  var faults = Array.prototype.slice.call(main.querySelectorAll('.fault'));
  var noFault = document.getElementById('noFault');
  var chips = Array.prototype.slice.call(document.querySelectorAll('.chip'));
  var activeCat = 'all';

  function applyFaultFilter() {
    var q = (faultSearch ? faultSearch.value : '').trim().toLowerCase();
    var shown = 0;

    faults.forEach(function (f) {
      var okCat = activeCat === 'all' || f.getAttribute('data-cat') === activeCat;
      var okText = !q || f.textContent.toLowerCase().indexOf(q) !== -1;
      var show = okCat && okText;
      f.classList.toggle('hide', !show);
      if (show) {
        shown++;
        if (q.length > 1) { f.open = true; }
      } else {
        f.open = false;
      }
    });

    if (noFault) { noFault.style.display = shown ? 'none' : 'block'; }
  }

  if (faultSearch) {
    faultSearch.addEventListener('input', applyFaultFilter);
  }

  chips.forEach(function (chip) {
    chip.addEventListener('click', function () {
      activeCat = chip.getAttribute('data-cat');
      chips.forEach(function (c) { c.classList.toggle('on', c === chip); });
      applyFaultFilter();
    });
  });

  /* ------------------------------------------------------------ 日志判定器 */
  var logInput = document.getElementById('logInput');
  var logResult = document.getElementById('logResult');
  var logCheckBtn = document.getElementById('logCheckBtn');
  var logSampleBtn = document.getElementById('logSampleBtn');
  var logClearBtn = document.getElementById('logClearBtn');

  var SAMPLE_LOG = [
    'I (24) boot: ESP-IDF v5.5.5-dirty 2nd stage bootloader',
    'I (44) boot.esp32s3: SPI Flash Size : 16MB',
    'I (226) esp_psram: Found 8MB PSRAM device',
    'I (676) cpu_start: GPIO 44 and 43 are used as console UART I/O pins',
    'I (676) cpu_start: cpu freq: 240000000 Hz',
    'I (682) app_init: Project name:     spider8',
    'I (686) app_init: App version:      0.1.0',
    'I (782) spider8: ==============================================',
    'I (788) spider8:   Spider8 四足蜘蛛机器人',
    'I (792) spider8:   固件   : spider8 v0.1.0 (build Oct  3 2026 10:00:50)',
    'I (799) spider8:   ESP-IDF: v5.5.5-dirty',
    'I (802) spider8:   芯片   : ESP32-S3 rev2, 2 核',
    'I (807) spider8:   Flash  : 16 MB',
    'I (810) spider8:   PSRAM  : 8192 KB',
    'I (813) spider8:   可用RAM: 419 KB',
    'I (822) spider8:   分区表:',
    'I (825) spider8:   [0] nvs      type=1 subtype=2   addr=0x009000 size=24KB',
    'I (846) spider8:   [3] ota_0    type=0 subtype=16  addr=0x020000 size=3072KB',
    'I (853) spider8:   [4] ota_1    type=0 subtype=17  addr=0x320000 size=3072KB',
    'I (861) spider8:   [5] littlefs type=1 subtype=130 addr=0x620000 size=10048KB',
    'I (868) spider8:   [6] coredump type=1 subtype=3   addr=0xff0000 size=64KB',
    'I (881) spider8: heartbeat #1, free heap=8764823, min-free=8732712',
    'I (1887) spider8: heartbeat #2, free heap=8769271, min-free=8732712',
    'I (2887) spider8: heartbeat #3, free heap=8769271, min-free=8732712',
    'I (3887) spider8: heartbeat #4, free heap=8769271, min-free=8732712'
  ].join('\n');

  function runLogCheck() {
    if (!logInput || !logResult) { return; }
    var t = logInput.value;

    if (!t.trim()) {
      logResult.className = 'result show';
      logResult.innerHTML = '<p class="muted" style="margin:0">先粘贴日志，或点「填入示例日志」看看合格输出长什么样。</p>';
      return;
    }

    var checks = [];

    function add(ok, title, detail) { checks.push({ ok: !!ok, title: title, detail: detail }); }

    /* 1. 是否抓到了应用日志 */
    var hasApp = /spider8:/.test(t);
    add(hasApp, '抓到应用日志', hasApp ? '检测到 spider8 的日志行' : '没看到 spider8: 前缀，可能抓的是引导阶段或串口选错了');

    /* 2. Flash */
    var mFlash = t.match(/Flash\s*[:：]\s*(\d+)\s*MB/i);
    if (mFlash) {
      var fmb = parseInt(mFlash[1], 10);
      add(fmb >= 16, 'Flash 容量', '检测到 ' + fmb + ' MB' + (fmb >= 16 ? '，符合 N16 规格' : '，预期 16 MB —— 板子可能不是 N16R8'));
    } else {
      var mBootFlash = t.match(/SPI Flash Size\s*[:：]\s*(\d+)\s*MB/i);
      if (mBootFlash) {
        add(parseInt(mBootFlash[1], 10) >= 16, 'Flash 容量', '引导程序报 ' + mBootFlash[1] + ' MB（应用横幅没抓到）');
      } else {
        add(false, 'Flash 容量', '没找到 "Flash : xx MB" 这一行');
      }
    }

    /* 3. PSRAM */
    var mPsram = t.match(/PSRAM\s*[:：]\s*(\d+)\s*KB/i);
    if (mPsram) {
      var pkb = parseInt(mPsram[1], 10);
      add(pkb >= 8192, 'PSRAM 容量', '检测到 ' + pkb + ' KB' + (pkb >= 8192 ? '，符合 R8 规格' : pkb === 0 ? ' —— 显示 0 说明 CONFIG_SPIRAM 没启用' : '，预期 8192 KB'));
    } else {
      add(/Found\s+\d+\s*MB\s+PSRAM/i.test(t), 'PSRAM 容量', '没找到 "PSRAM : xx KB"；引导阶段 ' + (/Found\s+\d+\s*MB\s+PSRAM/i.test(t) ? '识别到了 PSRAM，但应用侧没打印' : '也没识别到 PSRAM'));
    }

    /* 4. 芯片型号 */
    add(/ESP32-S3/i.test(t), '芯片型号', /ESP32-S3/i.test(t) ? '确认是 ESP32-S3' : '没检测到 ESP32-S3 标识');

    /* 5. IDF 版本 */
    var mIdf = t.match(/ESP-IDF\s+v?([\d]+\.[\d.]+)/i);
    add(!!mIdf, 'ESP-IDF 版本', mIdf ? 'v' + mIdf[1] : '没找到版本号');

    /* 6. 分区表 */
    var parts = ['ota_0', 'ota_1', 'littlefs', 'coredump'].filter(function (p) {
      return t.indexOf(p) !== -1;
    });
    add(parts.length === 4, '分区表完整性', parts.length === 4 ? 'ota_0 / ota_1 / littlefs / coredump 齐全' : '缺少：' + ['ota_0', 'ota_1', 'littlefs', 'coredump'].filter(function (p) { return t.indexOf(p) === -1; }).join('、'));

    /* 7. 心跳 */
    var hb = t.match(/heartbeat\s*#\d+/g) || [];
    add(hb.length >= 2, '心跳输出', hb.length >= 2 ? '抓到 ' + hb.length + ' 次心跳，调度器在跑' : '只抓到 ' + hb.length + ' 次，多等几秒再复制');

    /* 8. 内存水位 */
    var mins = [];
    var mfRe = /min-free[=:]\s*(\d+)/gi;
    var mf;
    while ((mf = mfRe.exec(t)) !== null) { mins.push(parseInt(mf[1], 10)); }
    if (mins.length >= 2) {
      var delta = Math.max.apply(null, mins) - Math.min.apply(null, mins);
      add(delta < 40960, '内存水位稳定', 'min-free 波动 ' + Math.round(delta / 1024) + ' KB' + (delta < 40960 ? '，未见泄漏' : '，波动偏大，留意是否有泄漏'));
    } else {
      add(false, '内存水位稳定', '没找到 min-free 字段，无法判断');
    }

    /* 9. 中文可读 */
    var garble = /\?{3,}|\uFFFD/.test(t);
    var hasCn = /蜘蛛|固件|四足|芯片/.test(t);
    if (garble) {
      add(false, '中文显示', '出现连续问号或替换字符 —— 是控制台代码页问题，不是固件问题');
    } else if (hasCn) {
      add(true, '中文显示', '中文正常渲染');
    } else {
      add(false, '中文显示', '没看到中文，可能只截了英文部分，或抓取时用了 ASCII 编码');
    }

    /* 渲染 */
    var pass = checks.filter(function (c) { return c.ok; }).length;
    var allPass = pass === checks.length;

    var html = '';
    checks.forEach(function (c) {
      html += '<div class="rline ' + (c.ok ? 'pass' : 'fail') + '">' +
                '<span class="ri">' + (c.ok ? '✓' : '✕') + '</span>' +
                '<div><span class="rt">' + c.title + '</span>' +
                '<span class="rd">' + escapeHtml(c.detail) + '</span></div>' +
              '</div>';
    });

    html += '<div class="rsum ' + (allPass ? 'pass' : 'fail') + '">' +
            (allPass
              ? '全部 ' + checks.length + ' 项通过 —— 板子规格与固件状态都符合预期，M0-1 验收完成。'
              : pass + ' / ' + checks.length + ' 项通过。未通过项按提示排查；若是抓取方式问题，先确认串口与编码。') +
            '</div>';

    logResult.className = 'result show';
    logResult.innerHTML = html;
  }

  if (logCheckBtn) { logCheckBtn.addEventListener('click', runLogCheck); }
  if (logSampleBtn) {
    logSampleBtn.addEventListener('click', function () {
      if (logInput) { logInput.value = SAMPLE_LOG; }
      runLogCheck();
      toast('已填入示例日志');
    });
  }
  if (logClearBtn) {
    logClearBtn.addEventListener('click', function () {
      if (logInput) { logInput.value = ''; }
      if (logResult) { logResult.className = 'result'; logResult.innerHTML = ''; }
      if (logInput) { logInput.focus(); }
    });
  }

  /* ------------------------------------------------------------ 滚动：进度条 / 高亮 / 回顶 */
  var readbar = document.getElementById('readbar');
  var toTop = document.getElementById('toTop');
  var marks = [];

  sections.forEach(function (sec) {
    marks.push({ id: sec.id, el: sec, parent: sec.id });
    Array.prototype.forEach.call(sec.querySelectorAll('h2'), function (h2) {
      marks.push({ id: h2.id, el: h2, parent: sec.id });
    });
  });

  var nsLinks = Array.prototype.slice.call(nav.querySelectorAll('a'));

  function setActive(id, parentId) {
    nsLinks.forEach(function (a) {
      var target = a.getAttribute('href').slice(1);
      a.classList.toggle('active', target === id);
    });

    Array.prototype.forEach.call(nav.children, function (li) {
      var isParent = li.getAttribute('data-for') === parentId;
      li.classList.toggle('open', isParent);
    });
  }

  var ticking = false;
  function onScroll() {
    if (ticking) { return; }
    ticking = true;
    window.requestAnimationFrame(function () {
      var doc = document.documentElement;
      var max = doc.scrollHeight - doc.clientHeight;
      var ratio = max > 0 ? doc.scrollTop / max : 0;
      if (readbar) { readbar.style.width = (ratio * 100).toFixed(2) + '%'; }
      if (toTop) { toTop.classList.toggle('show', doc.scrollTop > 700); }

      var line = window.innerHeight * 0.22;
      var current = marks[0];
      for (var i = 0; i < marks.length; i++) {
        var top = marks[i].el.getBoundingClientRect().top;
        if (top <= line) { current = marks[i]; } else { break; }
      }
      if (current) { setActive(current.id, current.parent); }

      ticking = false;
    });
  }

  window.addEventListener('scroll', onScroll, { passive: true });
  window.addEventListener('resize', onScroll);

  if (toTop) {
    toTop.addEventListener('click', function () {
      window.scrollTo({ top: 0, behavior: 'smooth' });
    });
  }

  /* ------------------------------------------------------------ 主题 */
  var themeBtn = document.getElementById('themeBtn');
  var savedTheme = store.get(KEY.theme, 'light');
  if (savedTheme === 'dark') { document.documentElement.setAttribute('data-theme', 'dark'); }

  function syncThemeIcon() {
    var dark = document.documentElement.getAttribute('data-theme') === 'dark';
    if (themeBtn) { themeBtn.title = dark ? '切换到浅色' : '切换到深色'; }
  }

  if (themeBtn) {
    themeBtn.addEventListener('click', function () {
      var dark = document.documentElement.getAttribute('data-theme') === 'dark';
      var next = dark ? 'light' : 'dark';
      document.documentElement.setAttribute('data-theme', next);
      store.set(KEY.theme, next);
      syncThemeIcon();
    });
  }
  syncThemeIcon();

  /* ------------------------------------------------------------ 移动端目录 */
  var menuBtn = document.getElementById('menuBtn');
  var sidebar = document.getElementById('sidebar');

  if (menuBtn) {
    menuBtn.addEventListener('click', function () {
      document.body.classList.toggle('nav-open');
    });
  }
  if (sidebar) {
    sidebar.addEventListener('click', function (e) {
      if (e.target.tagName === 'A') { document.body.classList.remove('nav-open'); }
    });
  }

  /* ------------------------------------------------------------ 快捷键 */
  document.addEventListener('keydown', function (e) {
    /* Ctrl/Cmd + K：跳到故障搜索 */
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
      e.preventDefault();
      var target = document.getElementById('troubleshoot');
      if (target) { target.scrollIntoView({ behavior: 'smooth' }); }
      setTimeout(function () { if (faultSearch) { faultSearch.focus(); } }, 380);
    }
    if (e.key === 'Escape') { document.body.classList.remove('nav-open'); }
  });

  /* ------------------------------------------------------------ 初始化 */
  refreshProgress();
  onScroll();

  if (window.location.hash) {
    var id = window.location.hash.slice(1);
    var target = document.getElementById(id);
    if (target) { setTimeout(function () { target.scrollIntoView(); }, 60); }
  }
})();
