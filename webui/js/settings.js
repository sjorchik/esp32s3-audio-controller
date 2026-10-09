'use strict';
/* Сторінка «Налаштування» (Prompt 23). Залежить від common.js і nav.js.
   Лише конфігурація пристрою (звук — на «Аудіо»). Кожне поле зберігається окремим POST /api/settings.
   Блок Wi-Fi — короткий підсумок поточної мережі; керування мережами — сторінка «Wi-Fi» (Prompt 29b). */
(() => {
  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), grp: g('grp'), proc: g('proc'), restart: g('restart-row'), restartText: g('restart-text'),
    btnRestart: g('btn-restart'), names: g('names'), btnNames: g('btn-names'),
    bright: g('bright'), brightOut: g('bright-out'), flip: g('flip'), wifiRows: g('wifi-rows'),
    inputsList: g('inputs-list'), inputsLoad: g('inputs-load'),
  };
  const CTRL = /[\u0000-\u001f\u007f]/;
  const CHIP = { Tda7318: 'TDA7318', Pt2313l: 'PT2313L' };
  const MODES = { OtaUpdate: 'Іде оновлення прошивки', IrLearn: 'Іде навчання пульта', WifiSetup: 'Пристрій налаштовує Wi-Fi' };

  let st = null;              // /api/status
  let cfg = null;             // /api/settings (збережені значення)
  let busy = false;           // триває збереження процесора / назв / перезапуск
  let flipBusy = false;       // триває запит перемикача: опитування його не чіпає
  let loadingCfg = false;
  let cfgFailed = false;
  const hideTimers = {};

  /* ---------- Дрібні помічники ---------- */
  function saved(key) {                                  // короткий «Збережено» біля поля
    const n = document.querySelector('[data-saved="' + key + '"]');
    if (!n) return;
    n.hidden = false;
    clearTimeout(hideTimers[key]);
    hideTimers[key] = setTimeout(() => { n.hidden = true; }, CONFIG.savedMs);
  }
  function setErr(f, t) {
    f.err.hidden = !t;
    setText(f.err, t);
    setAttr(f.input, 'aria-invalid', t ? 'true' : 'false');
  }
  function updCount(f) {
    const n = byteLength(f.input.value.trim());
    setText(f.cnt, n + ' / ' + CONFIG.inputNameMaxBytes + ' байт');
    f.cnt.className = (n > CONFIG.inputNameMaxBytes ? 'field-error' : 'hint') + ' num';
  }
  async function run(fn) {
    if (busy) return;
    busy = true; render();
    try { await fn(); } catch (e) { toast(errorText(e), 'error'); }
    busy = false; render();
  }
  const visibleInputs = () => CONFIG.processorInputs[ui.proc.value] || 4;

  /* ---------- Назви входів ---------- */
  const fields = [0, 1, 2, 3].map((i) => {
    const input = el('input', { class: 'input', type: 'text', id: 'in-' + i, autocomplete: 'off', spellcheck: 'false' });
    const cnt = el('small', { class: 'hint num' });
    const err = el('div', { class: 'field-error', role: 'alert', hidden: true });
    const wrap = el('div', { class: 'field' },
      el('label', { for: 'in-' + i }, 'Вхід ' + (i + 1) + (i === 0 ? ' (інтернет-радіо)' : '')), input, cnt, err);
    const f = { input, cnt, err, wrap, stored: '' };
    input.addEventListener('input', () => { updCount(f); setErr(f, ''); });
    ui.names.append(wrap);
    return f;
  });

  /* Дзеркало серверної валідації: байти UTF-8, не порожньо, без керівних символів. */
  function validateName(v) {
    if (!v) return 'Введіть назву.';
    if (CTRL.test(v)) return 'Назва містить керівні символи.';
    if (byteLength(v) > CONFIG.inputNameMaxBytes) {
      return 'Назва задовга: максимум ' + CONFIG.inputNameMaxBytes + ' байт (кирилична літера = 2 байти).';
    }
    return '';
  }

  function showNameErrors(e) {
    let shown = false;
    const list = e instanceof ApiError && Array.isArray(e.data.errors) ? e.data.errors : [];
    list.forEach((x) => {
      if (x.field === 'inputNames' && typeof x.index === 'number' && fields[x.index]) {
        setErr(fields[x.index], errorText(new ApiError(400, x, x.code)) + ' Максимум ' + (x.maxBytes || CONFIG.inputNameMaxBytes) + ' байт.');
        shown = true;
      }
    });
    if (!shown) toast(errorText(e), 'error');
  }

  async function saveNames() {
    if (busy || !cfg) return;
    const arr = [null, null, null, null];                 // null = не змінювати (web_api.md §3.2)
    let any = false, bad = false;
    fields.forEach((f, i) => {
      setErr(f, '');
      if (i >= visibleInputs()) return;
      const v = f.input.value.trim();
      const e = validateName(v);
      if (e) { setErr(f, e); if (!bad) f.input.focus(); bad = true; return; }
      f.input.value = v; updCount(f);
      if (v !== f.stored) { arr[i] = v; any = true; }
    });
    if (bad) return;
    if (!any) { toast('Назви не змінилися.'); return; }
    await run(async () => {
      try {
        await api.post('/api/settings', { inputNames: arr }, CONFIG.stationsWriteTimeoutMs);
        saved('names');
      } catch (e) { showNameErrors(e); }
      await loadSettings();
      loadInputs().catch(() => {});                       // назви в блоці «Входи» теж оновити
    });
  }
  ui.btnNames.addEventListener('click', saveNames);

  /* ---------- Тип процесора ---------- */
  ui.proc.addEventListener('change', async () => {
    const want = ui.proc.value;
    render();                                            // одразу оновити кількість полів назв
    await run(async () => {
      try {
        await api.post('/api/settings', { processorType: want }, CONFIG.stationsWriteTimeoutMs);
        cfg.processorType = want;
        saved('proc');
      } catch (e) {
        toast(errorText(e), 'error');
        ui.proc.value = cfg.processorType;
      }
    });
  });
  ui.btnRestart.addEventListener('click', async () => {
    if (busy) return;
    if (!(await confirmDialog('Перезапустити пристрій зараз? Звук зупиниться приблизно на 10–15 секунд.', { okText: 'Перезапустити', danger: true }))) return;
    run(() => rebootAndWait('/api/system/reboot', undefined, () => poller.stop()));
  });

  /* ---------- Входи (P39b; web_api.md §12) ---------- */
  let inputsBusy = false;     // триває POST /api/inputs: перемикачі заблоковані, checked не перезаписується
  const inRows = [0, 1, 2, 3].map((i) => {
    const box = el('input', { type: 'checkbox', id: 'inen-' + i });
    const name = el('span');
    const note = el('p', { class: 'hint', hidden: true });
    const wrap = el('div', { class: 'param', hidden: true }, el('label', { class: 'switch' }, box, name), note);
    box.addEventListener('change', () => toggleInput(i, box.checked));
    ui.inputsList.append(wrap);
    return { box, name, note, wrap };
  });

  function renderInputsCard() {
    const d = getInputs();
    ui.inputsLoad.hidden = !!d;
    inRows.forEach((r, i) => {
      const x = d && d.inputs.find((v) => v.index === i);
      r.wrap.hidden = !x;
      if (!x) return;
      const radio = i === 0;
      const avail = x.available !== false;
      setText(r.name, x.name || 'Вхід ' + (i + 1));
      if (!inputsBusy) r.box.checked = radio ? true : x.enabled !== false;
      r.box.disabled = radio || !avail || inputsBusy;
      const t = radio ? 'Радіо завжди доступне.' : !avail ? 'Недоступний для цього аудіопроцесора.' : '';
      setText(r.note, t);
      r.note.hidden = !t;
    });
  }

  async function toggleInput(i, want) {
    if (inputsBusy) return;
    const wasActive = !want && !!st && st.input === i;
    inputsBusy = true; renderInputsCard();
    try {
      await setInputEnabled(i, want);
      saved('inputs');
      if (wasActive) toast('Активний вхід вимкнено: пристрій перемкнувся на радіо.', 'ok');
    } catch (e) {
      toast(errorText(e), 'error');                       // 409 / 400 / 503: перемикач повернеться зі збереженого стану
    }
    inputsBusy = false;
    renderInputsCard();
    poller.now();
  }

  /* ---------- Дисплей ---------- */
  const brightCtl = createSlider(ui.bright, {
    send: async (v) => { await api.post('/api/settings', { brightness: v }); if (st) st.brightness = v; saved('bright'); },
    onInput: (v) => setText(ui.brightOut, v + '%'),
    onFail: () => render(),
  });
  ui.flip.addEventListener('change', async () => {
    const want = ui.flip.checked;
    flipBusy = true;
    try {
      await api.post('/api/settings', { displayFlipped: want });
      if (st) st.displayFlipped = want;
      saved('flip');
    } catch (e) {
      toast(errorText(e), 'error');
      ui.flip.checked = !want;
    }
    flipBusy = false;
    poller.now();
  });

  /* ---------- Wi-Fi (підсумок; керування — /wifi.html) ---------- */
  const wrows = [['Стан', 'state'], ['Мережа', 'ssid']].map(([label, key]) => {
    const td = el('td');
    ui.wifiRows.append(el('tr', null, el('td', { class: 'muted' }, label), td));
    return [key, td];
  });
  function renderWifiRows() {
    const w = st && st.wifi;
    const v = {
      state: !w ? '—' : w.connected ? 'Підключено' : w.apMode ? 'Точка доступу' : 'Не підключено',
      ssid: (w && w.ssid) || '—',
    };
    wrows.forEach(([k, td]) => setText(td, v[k]));
  }

  /* ---------- Дані ---------- */
  async function loadSettings() {
    if (loadingCfg) return;
    loadingCfg = true;
    try {
      const c = await api.get('/api/settings');
      const first = !cfg;
      cfg = c;
      cfgFailed = false;
      if (!busy || first) ui.proc.value = c.processorType;
      fields.forEach((f, i) => {
        const s = String((c.inputNames && c.inputNames[i]) || '');
        if (first || f.input.value === f.stored) f.input.value = s;     // не затираємо незбережену правку
        f.stored = s;
        updCount(f);
      });
    } catch (e) {
      if (!cfgFailed) toast('Налаштування: ' + errorText(e), 'error');
      cfgFailed = true;
    }
    loadingCfg = false;
    render();
  }

  async function refreshStatus() {
    st = await api.get('/api/status');
    syncInputs(st.input);
    if (!cfg) loadSettings();
    render();
  }
  const poller = createPoller(refreshStatus, { intervalMs: CONFIG.settingsPollMs });

  /* ---------- Рендер ---------- */
  function render() {
    const m = st && MODES[st.mode];
    setBanner('busy', m ? m + '. Деякі зміни пристрій може відхилити.' : null, 'warn');
    if (st) renderWifi(ui.wifi, st.wifi);
    ui.grp.disabled = !st || !cfg || connection.offline;
    ui.proc.disabled = busy;
    ui.btnNames.disabled = busy;
    ui.btnRestart.disabled = busy;
    const n = visibleInputs();
    fields.forEach((f, i) => { f.wrap.hidden = i >= n; });

    const now = st && st.processor && st.processor.type;
    const need = !!now && !!cfg && now !== cfg.processorType;      // збережений тип ≠ тип, з яким працює прошивка
    ui.restart.hidden = !need;
    if (need) {
      setText(ui.restartText, 'Тип змінено на ' + (CHIP[cfg.processorType] || cfg.processorType) +
        ', але пристрій працює з ' + (CHIP[now] || now) + '. Зміна застосується після перезапуску.');
    }
    if (st) {
      if (!brightCtl.held()) { ui.bright.value = st.brightness; setText(ui.brightOut, st.brightness + '%'); }
      if (!flipBusy) ui.flip.checked = !!st.displayFlipped;
    }
    renderWifiRows();
    renderInputsCard();
  }

  initShell('settings');
  onConnectionChange(render);
  onInputsChange(renderInputsCard);
  render();
  loadSettings();
  poller.start();
})();
