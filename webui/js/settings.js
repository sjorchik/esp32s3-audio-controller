'use strict';
/* Сторінка «Налаштування» (Prompt 23). Залежить від common.js і nav.js.
   Лише конфігурація пристрою (звук — на «Аудіо»). Назви входів — масив рівно з 3 елементів; яскравість і
   displayFlipped — окремі POST /api/settings (P51a: тип процесора й тембр прибрано).
   Блок «Входи» (V3): у кожному рядку — галочка активації входу (POST /api/inputs) і повзунок чутливості VU
   (GET / POST /api/vu, web_api.md §15). Блок Wi-Fi — короткий підсумок поточної мережі; керування мережами —
   сторінка «Wi-Fi» (Prompt 29b). */
(() => {
  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), grp: g('grp'), names: g('names'), btnNames: g('btn-names'),
    bright: g('bright'), brightOut: g('bright-out'), flip: g('flip'), wifiRows: g('wifi-rows'),
    inputsList: g('inputs-list'), inputsLoad: g('inputs-load'),
  };
  const CTRL = /[\u0000-\u001f\u007f]/;
  const MODES = { OtaUpdate: 'Іде оновлення прошивки', IrLearn: 'Іде навчання пульта', WifiSetup: 'Пристрій налаштовує Wi-Fi' };
  const isNum = (n) => typeof n === 'number' && isFinite(n);

  let st = null;              // /api/status
  let cfg = null;             // /api/settings (збережені значення)
  let busy = false;           // триває збереження назв
  let flipBusy = false;       // триває запит перемикача: опитування його не чіпає
  let loadingCfg = false;
  let cfgFailed = false;
  let vu = null;              // {minDb, maxDb, stepDb, vals[3]} з GET /api/vu; null = ще не прочитано
  let vuLoading = false;
  let vuFailed = false;
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

  /* ---------- Назви входів ---------- */
  const INPUT_LABELS = ['Вхід 0 · інтернет-радіо', 'Вхід 1', 'Вхід 2'];
  const fields = Array.from({ length: CONFIG.inputCount }, (_, i) => {
    const input = el('input', { class: 'input', type: 'text', id: 'in-' + i, autocomplete: 'off', spellcheck: 'false',
      placeholder: CONFIG.inputDefaultNames[i] || '' });
    const cnt = el('small', { class: 'hint num' });
    const err = el('div', { class: 'field-error', role: 'alert', hidden: true });
    const wrap = el('div', { class: 'field' },
      el('label', { for: 'in-' + i }, INPUT_LABELS[i] || 'Вхід ' + i), input, cnt, err);
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
    const arr = new Array(CONFIG.inputCount).fill(null);  // рівно 3 елементи; null = не змінювати (web_api.md §3.2)
    let any = false, bad = false;
    fields.forEach((f, i) => {
      setErr(f, '');
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

  /* ---------- Входи + чутливість VU (V3; web_api.md §12, §15) ---------- */
  let inputsBusy = false;     // триває POST /api/inputs: галочки заблоковані, checked не перезаписується
  const fmtVu = (v) => fmtSigned(v) + ' дБ';
  // Чутливість активного входу недоступна в standby (409 standby з POST /api/vu).
  const sensLocked = (i) => !!st && i === st.input && (st.standby === true || st.mode === 'Standby');

  const inRows = Array.from({ length: CONFIG.inputCount }, (_, i) => {
    const box = el('input', { type: 'checkbox', id: 'inen-' + i });
    const name = el('span');
    const note = el('p', { class: 'hint', hidden: true });
    const out = el('output', { class: 'param-val num' }, fmtVu(0));
    const input = el('input', { class: 'slider slider-c', type: 'range', min: CONFIG.vuSensMinDb, max: CONFIG.vuSensMaxDb,
      step: CONFIG.vuSensStepDb, value: 0, 'aria-label': 'Чутливість VU: вхід ' + (i + 1) });
    const zero = el('button', { class: 'btn btn-zero', type: 'button', 'aria-label': 'Скинути чутливість VU' }, '0');
    const r = { box, name, note, input, out, zero, wrap: null, ctl: null, set: null };
    r.ctl = createSlider(input, {
      send: async (v) => applyVu(await api.setVu(v, i), i),
      onInput: (v) => setText(out, fmtVu(v)),
      onFail: () => renderInputsCard(),
    });
    r.set = (v) => {
      v = clamp(v, Number(input.min), Number(input.max));
      input.value = v; setText(out, fmtVu(v));
      r.ctl.send(v);
    };
    zero.addEventListener('click', () => r.set(0));
    box.addEventListener('change', () => toggleInput(i, box.checked));
    r.wrap = el('div', { class: 'param in-row', hidden: true },
      el('label', { class: 'switch' }, box, name),
      el('div', { class: 'param-row' }, input, out, zero),
      note);
    ui.inputsList.append(r.wrap);
    return r;
  });

  function renderInputsCard() {
    const d = getInputs();
    ui.inputsLoad.hidden = !!d;
    inRows.forEach((r, i) => {
      const x = d && d.inputs.find((v) => v.index === i);
      r.wrap.hidden = !x;
      if (!x) return;
      const radio = i === 0;
      const active = !!st && st.input === i;
      const avail = x.available !== false;

      // Галочка активації (як раніше)
      setText(r.name, x.name || CONFIG.inputDefaultNames[i] || 'Вхід ' + i);
      if (!inputsBusy) r.box.checked = radio ? true : x.enabled !== false;
      r.box.disabled = radio || x.canDisable === false || !avail || inputsBusy;

      // Повзунок чутливості VU
      if (vu) {
        setAttr(r.input, 'min', vu.minDb);             // min / max / step — ПЕРЕД value
        setAttr(r.input, 'max', vu.maxDb);
        setAttr(r.input, 'step', vu.stepDb);
        if (!r.ctl.held()) { r.input.value = vu.vals[i]; setText(r.out, fmtVu(vu.vals[i])); }
      }
      const off = !vu || sensLocked(i);
      r.input.disabled = off;
      r.zero.disabled = off;

      // Пояснення: лише там, де воно справді потрібне
      const notes = [];
      if (radio) notes.push('Радіо завжди доступне.');
      else if (!avail) notes.push('Вхід апаратно недоступний.');
      if (vu && !active) notes.push('Не активний вхід: чутливість діє після перемикання на нього.');
      if (sensLocked(i)) notes.push('У режимі очікування чутливість активного входу не змінюється.');
      setText(r.note, notes.join(' '));
      r.note.hidden = !notes.length;
    });
  }

  function applyVu(d, i) {
    if (vu && d && isNum(d.sensitivityDb) && (!isNum(d.input) || d.input === i)) vu.vals[i] = d.sensitivityDb;
    renderInputsCard();
  }

  function applyVuState(d) {
    if (!d || !Array.isArray(d.inputs)) throw new ApiError(200, null, 'bad_response');
    const vals = new Array(CONFIG.inputCount).fill(0);
    d.inputs.forEach((x) => {
      if (x && isNum(x.index) && x.index >= 0 && x.index < CONFIG.inputCount && isNum(x.sensitivityDb)) {
        vals[x.index] = x.sensitivityDb;
      }
    });
    vu = {
      minDb: isNum(d.minDb) ? d.minDb : CONFIG.vuSensMinDb,
      maxDb: isNum(d.maxDb) ? d.maxDb : CONFIG.vuSensMaxDb,
      stepDb: isNum(d.stepDb) && d.stepDb > 0 ? d.stepDb : CONFIG.vuSensStepDb,
      vals,
    };
  }

  async function loadVu() {
    if (vuLoading) return;
    vuLoading = true;
    try {
      applyVuState(await api.get('/api/vu'));
      vuFailed = false;
    } catch (e) {
      if (!vuFailed) toast('Чутливість VU: ' + errorText(e), 'error');
      vuFailed = true;
    }
    vuLoading = false;
    renderInputsCard();
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
      toast(errorText(e), 'error');                       // 409 / 400 / 503: галочка повернеться зі збереженого стану
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
    if (!vu) loadVu();                                    // чутливість читаємо, доки не отримали; далі — з відповідей POST
    render();
  }
  const poller = createPoller(refreshStatus, { intervalMs: CONFIG.settingsPollMs });

  /* ---------- Рендер ---------- */
  function render() {
    const m = st && MODES[st.mode];
    setBanner('busy', m ? m + '. Деякі зміни пристрій може відхилити.' : null, 'warn');
    if (st) renderWifi(ui.wifi, st.wifi);
    ui.grp.disabled = !st || !cfg || connection.offline;
    ui.btnNames.disabled = busy;
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
  loadVu();
  poller.start();
})();