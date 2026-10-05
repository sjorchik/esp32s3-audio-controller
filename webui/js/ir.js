'use strict';
/* Сторінка «Пульт» (Prompt 22). Залежить від common.js і nav.js.
   Навчання виконує пристрій (режим IrLearn); сторінка ініціює його й опитує /api/ir/learn/status.
   Таймаути навчання (Waiting / Conflict) на пристрої є: статус Timeout лишається, доки не буде cancel / нове навчання. */
(() => {
  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), grp: g('grp'), count: g('count'), only: g('only-new'), file: g('file'),
    groups: g('groups'), empty: g('empty'),
    learn: g('learn'), badge: g('l-badge'), origin: g('l-origin'), target: g('l-target'), text: g('l-text'),
    over: g('l-over'), cancel: g('l-cancel'),
  };

  /* Групи та українські назви; ключ — імʼя дії з API. Невідома дія → «Інше» з сирим імʼям. */
  const GROUPS = [['power', 'Живлення'], ['vol', 'Гучність і мʼют'], ['input', 'Вхід'], ['play', 'Станції й плеєр'],
    ['tone', 'Тембр, баланс, підсилення'], ['nav', 'Навігація'], ['other', 'Інше']];
  const NAMES = {
    POWER: ['Живлення', 'power'],
    VOL_UP: ['Гучніше', 'vol'], VOL_DOWN: ['Тихіше', 'vol'], MUTE: ['Вимкнути звук', 'vol'],
    INPUT_RADIO: ['Вхід: WiFi Radio', 'input'], INPUT_TV: ['Вхід: TV Box', 'input'],
    INPUT_PC: ['Вхід: Computer', 'input'], INPUT_AUX: ['Вхід: Aux', 'input'],
    BASS_UP: ['Бас +', 'tone'], BASS_DOWN: ['Бас \u2212', 'tone'],
    TREBLE_UP: ['Дискант +', 'tone'], TREBLE_DOWN: ['Дискант \u2212', 'tone'],
    BALANCE_UP: ['Баланс +', 'tone'], BALANCE_DOWN: ['Баланс \u2212', 'tone'],
    GAIN_UP: ['Підсилення +', 'tone'], GAIN_DOWN: ['Підсилення \u2212', 'tone'],
    UP: ['Вгору', 'nav'], DOWN: ['Вниз', 'nav'], LEFT: ['Вліво', 'nav'], RIGHT: ['Вправо', 'nav'],
    OK: ['OK', 'nav'], BACK: ['Назад', 'nav'], MENU: ['Меню', 'nav'],
  };
  function info(a) {
    if (NAMES[a]) return { n: NAMES[a][0], g: NAMES[a][1] };
    const m = /^DIGIT_(\d)$/.exec(a);
    return m ? { n: 'Цифра ' + m[1], g: 'play' } : { n: a, g: 'other' };
  }
  const lbl = (a) => (a ? info(a).n : 'іншою дією');

  /* Стани LearnStatus (web_api.md §6.5): [бейдж, вигляд бейджа, підказка] */
  const LSTAT = {
    Waiting: ['Очікування', 'warn', 'Натисніть кнопку на пульті.'],
    Confirm: ['Код прийнято', 'warn', 'Натисніть ту саму кнопку ще раз для підтвердження.'],
    Conflict: ['Конфлікт', 'err', ''],
    Success: ['Готово', 'ok', 'Кнопку навчено.'],
    Timeout: ['Час вичерпано', 'err', 'Кнопку не натиснули вчасно.'],
    Idle: ['Завершення', '', 'Навчання завершується…'],
  };
  const fin = (s) => s === 'Success' || s === 'Timeout';

  let st = null;            // /api/status
  let actions = [];         // /api/ir/actions
  let rows = [];            // [{li, a}]
  let secs = [];            // [{sec, items:[li]}]
  let loaded = false, loadError = false, loading = false, again = false, nextTry = 0;
  let busy = false;
  let session = null;       // {target}: навчання запущено з цієї сторінки
  let ls = null;            // останній /api/ir/learn/status
  let finalDone = true;     // результат Success/Timeout уже оброблено
  let learnOn = false;

  const devLearn = () => !!st && st.mode === 'IrLearn';
  const learning = () => session !== null || devLearn();
  const learnedCount = () => actions.filter((a) => a.learned).length;
  function lockKind() {
    if (!st) return 'loading';
    if (connection.offline) return 'offline';
    if (st.mode === 'OtaUpdate') return 'ota';
    return null;
  }

  /* ---------- Мапа ---------- */
  function buildMap() {
    const by = new Map(GROUPS.map(([id, t]) => [id, { t, a: [] }]));
    actions.forEach((a) => by.get(info(a.action).g).a.push(a));
    rows = []; secs = [];
    by.forEach(({ t, a: list }) => {
      if (!list.length) return;
      const ul = el('ul', { class: 'station-list' });
      const items = [];
      list.forEach((a) => {
        const n = info(a.action).n;
        const code = a.learned ? 'адр. ' + a.addr + ' · ком. ' + a.cmd : 'не навчено';
        const del = a.learned && el('button', { class: 'btn btn-ghost btn-icon', type: 'button', 'data-act': 'clear', 'aria-label': 'Очистити «' + n + '»' });
        if (del) setIcon(del, 'trash');
        const li = el('li', { class: 'station', 'data-action': a.action },
          el('div', { class: 'station-main' },
            el('div', { class: 'station-text' },
              el('div', { class: 'station-name' }, n),
              el('div', { class: 'station-url muted num' }, (n !== a.action ? a.action + ' · ' : '') + code))),
          el('div', { class: 'station-actions' },
            el('span', { class: 'badge' + (a.learned ? ' badge-ok' : '') }, a.learned ? 'Навчено' : 'Не навчено'),
            el('button', { class: 'btn' + (a.learned ? '' : ' btn-primary'), type: 'button', 'data-act': 'learn' }, a.learned ? 'Перенавчити' : 'Навчити'),
            del));
        ul.append(li); items.push(li); rows.push({ li, a });
      });
      const done = list.filter((a) => a.learned).length;
      const sec = el('section', { class: 'card' },
        el('div', { class: 'card-head' }, el('h2', { class: 'card-title' }, t), el('span', { class: 'muted num' }, done + ' з ' + list.length)), ul);
      secs.push({ sec, items });
    });
    ui.groups.replaceChildren(...secs.map((s) => s.sec));
  }

  function renderMap() {
    const only = ui.only.checked;
    rows.forEach((r) => { r.li.hidden = only && r.a.learned; });
    secs.forEach((s) => { s.sec.hidden = s.items.every((li) => li.hidden); });
    setText(ui.count, loaded ? '· навчено ' + learnedCount() + ' з ' + actions.length : '');
    let t = null;
    if (!loaded) t = loadError ? 'Не вдалося завантажити мапу. Повторюємо спроби…' : 'Завантаження…';
    else if (!actions.length) t = 'Пристрій не повернув жодної дії.';
    else if (only && learnedCount() === actions.length) t = 'Усі кнопки навчено.';
    ui.empty.hidden = t === null;
    if (t !== null) setText(ui.empty, t);
  }

  async function loadMap() {
    if (loading) { again = true; return; }
    loading = true;
    try {
      const r = await api.get('/api/ir/actions', CONFIG.stationsTimeoutMs);
      actions = Array.isArray(r) ? r : [];
      loaded = true; loadError = false;
      buildMap();
    } catch (e) {
      loadError = true; nextTry = Date.now() + CONFIG.stationsRetryMs;
      if (loaded) toast(errorText(e), 'error');
    }
    loading = false;
    if (again) { again = false; loadMap(); return; }
    render();
  }

  /* Запис / довга дія: блокує сторінку, потім перечитує мапу. */
  async function write(fn, reload) {
    if (busy) return;
    busy = true; setBanner('import-result', null); render();
    try { await fn(); } catch (e) { toast(errorText(e), 'error'); }
    busy = false;
    if (reload !== false) await loadMap();
    statusPoller.now(); render();
  }

  async function clearCode(action) {
    const ok = await confirmDialog('Очистити код кнопки «' + lbl(action) + '»? Пульт перестане її підтримувати.', { okText: 'Очистити', danger: true });
    if (!ok) return;
    write(async () => {
      await api.del('/api/ir/map/' + encodeURIComponent(action), CONFIG.stationsWriteTimeoutMs);
      toast('Код очищено.', 'ok');
    });
  }

  function exportMap() {
    write(async () => {
      const b = await api.blob('/api/ir/map', CONFIG.stationsTimeoutMs);
      downloadBlob(b, 'ir-map-' + fileStamp() + '.json');
    }, false);
  }

  ui.file.addEventListener('change', async () => {
    const f = ui.file.files[0];
    ui.file.value = '';
    if (!f) return;
    if (f.size === 0) { toast(REASONS.empty, 'error'); return; }
    if (f.size > CONFIG.irImportMaxBytes) {
      toast('Файл завеликий: ' + formatBytes(f.size) + ' (максимум ' + formatBytes(CONFIG.irImportMaxBytes) + ').', 'error');
      return;
    }
    let text, n;
    try {
      text = await f.text();
      const d = JSON.parse(text);
      if (!Array.isArray(d)) throw new Error('not array');
      n = d.length;
    } catch (_) { toast('Файл має бути JSON-масивом у форматі експорту пульта.', 'error'); return; }
    const ok = await confirmDialog('Імпорт замінить усю мапу пульта (зараз навчено ' + learnedCount() + ') вмістом файлу «' + f.name +
      '» (записів: ' + n + '). Скасувати це буде неможливо. Продовжити?', { okText: 'Замінити', danger: true });
    if (!ok) return;
    write(async () => {
      try {
        const r = await api.send('POST', '/api/ir/map/import', text, 'application/json', CONFIG.stationsWriteTimeoutMs);
        setBanner('import-result', 'Імпорт завершено: у мапі кодів — ' + r.count + '. Попередню мапу замінено.', 'info');
      } catch (e) {
        setBanner('import-result', 'Імпорт не вдався: ' + errorText(e) + ' Нижче — актуальна мапа з пристрою.', 'warn');
      }
    });
  });

  /* ---------- Навчання ---------- */
  function applyLearn(r) {
    ls = r;
    if (fin(r.status)) {
      if (finalDone) return;
      finalDone = true;
      if (learning()) {
        if (r.status === 'Success') { toast('Кнопку «' + lbl(r.target) + '» навчено.', 'ok'); loadMap(); }
        else toast('Час очікування вичерпано. Кнопку не навчено.', 'error');
      }
      session = null;
    } else {
      finalDone = false;
      if (r.status === 'Idle' && session) { session = null; toast('Навчання завершено без результату.'); }
    }
  }

  async function startLearn(action) {
    if (busy || learning()) return;
    busy = true; render();
    try {
      const r = await api.post('/api/ir/learn', { action });
      finalDone = false; session = { target: action };
      applyLearn(r);
      busy = false; render();
      ui.learn.scrollIntoView({ block: 'center' });
    } catch (e) {
      toast(errorText(e), 'error');
      busy = false; render();
    }
    statusPoller.now();
  }

  ui.cancel.addEventListener('click', async () => {
    try { await api.post('/api/ir/learn/cancel'); } catch (e) { toast(errorText(e), 'error'); return; }
    session = null; finalDone = false;
    ls = { status: 'Idle', target: null, conflictWith: null };
    toast('Навчання скасовано.');
    render(); statusPoller.now();
  });

  ui.over.addEventListener('click', async () => {
    const s = ls;
    if (!s || s.status !== 'Conflict') return;
    const ok = await confirmDialog('Код уже закріплено за «' + lbl(s.conflictWith) + '». Закріпити його за «' + lbl(s.target) +
      '»? Для «' + lbl(s.conflictWith) + '» код буде видалено.', { okText: 'Перезаписати', danger: true });
    if (!ok) return;
    ui.over.disabled = true;
    try { await api.post('/api/ir/learn/confirm-overwrite'); } catch (e) { toast(errorText(e), 'error'); }
    ui.over.disabled = false;
    learnPoller.now();
  });

  /* ---------- Рендер ---------- */
  function renderPanel(on) {
    ui.learn.hidden = !on;
    if (!on) return;
    const s = ls ? ls.status : null;
    const d = LSTAT[s] || ['…', '', s ? 'Стан: ' + s : 'Отримуємо стан навчання…'];
    ui.badge.className = 'badge' + (d[1] ? ' badge-' + d[1] : '');
    setText(ui.badge, d[0]);
    const tgt = (ls && ls.target) || (session && session.target) || null;
    setText(ui.target, tgt ? lbl(tgt) : '—');
    setText(ui.text, s === 'Conflict'
      ? 'Цей код уже закріплено за «' + lbl(ls.conflictWith) + '». Перезаписати його чи скасувати?' : d[2]);
    ui.over.hidden = s !== 'Conflict';
    ui.origin.hidden = session !== null;
  }

  function renderBanners(lock) {
    setBanner('ota', lock === 'ota' ? 'Оновлення прошивки: ' + (st.otaProgress || 0) + '%. Керування вимкнено.' : null,
      'warn', { progress: st && st.otaProgress });
    setBanner('irlearn', devLearn() && !session ? 'Пристрій у режимі навчання пульта. Решта дій вимкнена.' : null, 'warn');
  }

  function render() {
    const lock = lockKind();
    renderBanners(lock);
    if (st) renderWifi(ui.wifi, st.wifi);
    const panelOn = session !== null || (devLearn() && !(ls && (fin(ls.status) || ls.status === 'Idle')));
    renderPanel(panelOn);
    const need = learning();                       // опитуємо статус навчання лише поки воно триває
    if (need && !learnOn) { learnOn = true; learnPoller.start(); }
    else if (!need && learnOn) { learnOn = false; learnPoller.stop(); }
    ui.grp.disabled = lock !== null || need || busy;
    ui.grp.classList.toggle('is-busy', busy && lock === null && !need);
    renderMap();
  }

  /* ---------- Опитування ---------- */
  async function refreshStatus() {
    st = await api.get('/api/status');
    if (!loaded && !loading && Date.now() >= nextTry) loadMap();
    render();
  }
  const statusPoller = createPoller(refreshStatus, { intervalMs: CONFIG.irStatusPollMs });
  const learnPoller = createPoller(async () => { applyLearn(await api.get('/api/ir/learn/status')); render(); },
    { intervalMs: CONFIG.irLearnPollMs });

  ui.only.addEventListener('change', renderMap);
  ui.grp.addEventListener('click', (ev) => {
    const b = ev.target.closest('button[data-act]');
    if (!b) return;
    const li = b.closest('li[data-action]');
    const a = li && li.dataset.action;
    const act = {
      refresh: () => { loadMap(); statusPoller.now(); },
      import: () => ui.file.click(),
      export: exportMap,
      learn: () => startLearn(a),
      clear: () => clearCode(a),
    }[b.dataset.act];
    if (act) act();
  });

  initShell('ir');
  onConnectionChange(render);
  render();
  loadMap();
  statusPoller.start();
})();
