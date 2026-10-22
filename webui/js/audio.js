'use strict';
/* Сторінка «Аудіо» (P51b): еквалайзер (5 смуг) і баланс ДЛЯ КОЖНОГО з 3 входів.
   Залежить від common.js і nav.js. Контракт: web_api.md §11 (/api/eq), §13 (/api/balance), §1.3 (профілі входів).
   Вкладка вибирає лише профіль для редагування — вхід на пристрої НЕ перемикається.
   Для кожного входу створено власний набір повзунків (createSlider): відкладена відправка з повзунка
   ніколи не потрапить у профіль іншого входу після перемикання вкладки. */
(() => {
  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), grp: g('grp'), tabs: g('tabs'), scope: g('scope'), reset: g('btn-reset-input'),
    eqPanels: g('eq-panels'), eqErr: g('eq-err'), balPanels: g('bal-panels'), balNote: g('bal-note'), balErr: g('bal-err'),
  };
  const INPUTS = CONFIG.inputCount;
  const BANDS = CONFIG.eqBandFreqHz.length;     // кількість смуг (довжина gainsDb); частоти й межі — з відповіді

  let st = null;             // останній /api/status
  let selected = null;       // вхід, чий профіль редагується (НЕ обовʼязково активний на пристрої)
  let tabsSig = '';
  let resetBusy = false;
  // Кеш профілів: eq = {gainsDb, freqHz, minDb, maxDb, stepDb}, bal = число; null = ще не прочитано
  const prof = Array.from({ length: INPUTS }, () => ({ eq: null, bal: null }));
  const panels = [];

  const isNum = (n) => typeof n === 'number' && isFinite(n);
  const isConn = (e) => e instanceof ApiError && (e.code === 'network' || e.code === 'timeout');
  const fmtDb = (v) => fmtSigned(v) + ' дБ';
  const fmtFreq = (f) => (f >= 1000 ? String(f / 1000).replace('.', ',') + ' кГц' : f + ' Гц');
  const iconBtn = (name, label, fn) => {
    const b = el('button', { class: 'btn btn-icon', type: 'button', 'aria-label': label, onclick: fn });
    setIcon(b, name);
    return b;
  };
  const isStandby = () => !!st && (st.standby === true || st.mode === 'Standby');
  const nameOf = (i) => inputName(i, st && st.inputs);
  function setErr(node, text) {
    setText(node, text || '');
    node.hidden = !text;
  }

  /* Блокування всієї сторінки: немає звʼязку або іде OTA (409 ota_in_progress для будь-якого входу). */
  function lockKind() {
    if (!st) return 'loading';
    if (connection.offline) return 'offline';
    if (st.mode === 'OtaUpdate') return 'ota';
    return null;
  }
  /* Баланс АКТИВНОГО входу недоступний у standby / під час навчання пульта (409 standby | ir_learn_active);
     профіль неактивного входу змінюється завжди (крім OTA). */
  function balLocked(i) {
    return !!st && i === st.input && (isStandby() || st.mode === 'IrLearn');
  }

  /* ---------- Побудова повзунків (по набору на кожен вхід) ---------- */
  function buildPanel(i) {
    const eqLegend = el('legend', { class: 'visually-hidden' });
    const eqSet = el('fieldset', { class: 'group', disabled: true }, eqLegend);
    const rows = [];
    for (let b = 0; b < BANDS; b++) {
      const input = el('input', { class: 'slider slider-c', type: 'range', min: CONFIG.eqMinDb, max: CONFIG.eqMaxDb,
        step: CONFIG.eqStepDb, value: 0, 'aria-label': 'Смуга ' + (b + 1) });
      const title = el('span', null, 'Смуга ' + (b + 1));
      const val = el('output', { class: 'param-val num' }, fmtDb(0));
      const r = { b, input, title, val };
      r.ctl = createSlider(input, {
        send: async (v) => { applyEq(i, await api.setEq({ band: b, gainDb: v }, i)); },
        onInput: (v) => setText(val, fmtDb(v)),
        onFail: () => renderPanel(i),
      });
      r.set = (v) => {
        v = clamp(v, Number(input.min), Number(input.max));
        input.value = v; setText(val, fmtDb(v));
        r.ctl.send(v);
      };
      const step = () => Number(input.step) || 1;
      r.row = el('div', { class: 'param' },
        el('div', { class: 'param-head' }, title, val),
        el('div', { class: 'param-row' },
          iconBtn('minus', 'Менше', () => r.set(Number(input.value) - step())),
          input,
          iconBtn('plus', 'Більше', () => r.set(Number(input.value) + step())),
          el('button', { class: 'btn btn-zero', type: 'button', 'aria-label': 'Скинути смугу', onclick: () => r.set(0) }, '0')));
      eqSet.append(r.row);
      rows.push(r);
    }
    const eqBox = el('div', { class: 'in-panel', hidden: true }, eqSet);
    ui.eqPanels.append(eqBox);

    const balLegend = el('legend', { class: 'visually-hidden' });
    const balSet = el('fieldset', { class: 'group', disabled: true }, balLegend);
    const balInput = el('input', { class: 'slider slider-c', type: 'range', min: CONFIG.balanceMinDb, max: CONFIG.balanceMaxDb,
      step: CONFIG.balanceStepDb, value: CONFIG.balanceNeutralDb, 'aria-label': 'Баланс' });
    const balVal = el('output', { class: 'param-val num' }, fmtDb(CONFIG.balanceNeutralDb));
    const bal = { input: balInput, val: balVal, fs: balSet, legend: balLegend };
    bal.ctl = createSlider(balInput, {
      send: async (v) => { applyBal(i, await api.setBalance(v, i)); },
      onInput: (v) => setText(balVal, fmtDb(v)),
      onFail: () => renderPanel(i),
    });
    bal.set = (v) => {
      v = clamp(v, Number(balInput.min), Number(balInput.max));
      balInput.value = v; setText(balVal, fmtDb(v));
      bal.ctl.send(v);
    };
    const bStep = () => Number(balInput.step) || 1;
    balSet.append(el('div', { class: 'param' },
      el('div', { class: 'param-head' }, el('span', null, 'Баланс'), balVal),
      el('div', { class: 'param-row' },
        iconBtn('minus', 'Баланс: вліво', () => bal.set(Number(balInput.value) - bStep())),
        el('div', { class: 'bal-wrap' }, balInput,
          el('div', { class: 'bal-scale', 'aria-hidden': 'true' }, el('span', null, 'Л'), el('span', null, '0'), el('span', null, 'П'))),
        iconBtn('plus', 'Баланс: вправо', () => bal.set(Number(balInput.value) + bStep())),
        el('button', { class: 'btn btn-zero', type: 'button', 'aria-label': 'Скинути баланс', onclick: () => bal.set(CONFIG.balanceNeutralDb) }, '0'))));
    const balBox = el('div', { class: 'in-panel', hidden: true }, balSet);
    ui.balPanels.append(balBox);

    return { eq: { box: eqBox, fs: eqSet, legend: eqLegend, rows }, bal: Object.assign(bal, { box: balBox }) };
  }
  for (let i = 0; i < INPUTS; i++) panels.push(buildPanel(i));

  /* ---------- Прийом даних ---------- */
  // Відповідь GET / POST /api/eq?input=i: значення, а також межі й частоти (контракт: брати з відповіді).
  function applyEq(i, d) {
    if (!d || !Array.isArray(d.gainsDb) || d.gainsDb.length !== BANDS || !d.gainsDb.every(isNum)) return;
    if (isNum(d.input) && d.input !== i) return;
    const old = prof[i].eq;
    prof[i].eq = {
      gainsDb: d.gainsDb.slice(),
      freqHz: Array.isArray(d.freqHz) && d.freqHz.length === BANDS && d.freqHz.every(isNum) ? d.freqHz.slice() : (old && old.freqHz) || null,
      minDb: isNum(d.minDb) ? d.minDb : old ? old.minDb : CONFIG.eqMinDb,
      maxDb: isNum(d.maxDb) ? d.maxDb : old ? old.maxDb : CONFIG.eqMaxDb,
      stepDb: isNum(d.stepDb) && d.stepDb > 0 ? d.stepDb : old ? old.stepDb : CONFIG.eqStepDb,
    };
    renderPanel(i);
  }
  // Відповідь GET {input, balance} / POST {ok, input, balance}.
  function applyBal(i, d) {
    if (!d || !isNum(d.balance)) return;
    if (isNum(d.input) && d.input !== i) return;
    prof[i].bal = d.balance;
    renderPanel(i);
  }

  async function loadProfile(i) {
    let eqErr = null, balErr = null;
    try { applyEq(i, await api.getEq(i)); } catch (e) { if (isConn(e)) throw e; eqErr = errorText(e); }
    try { applyBal(i, await api.getBalance(i)); } catch (e) { if (isConn(e)) throw e; balErr = errorText(e); }
    if (i === selected) { setErr(ui.eqErr, eqErr); setErr(ui.balErr, balErr); }
  }

  /* ---------- Рендер ---------- */
  function renderPanel(i) {
    const p = panels[i], c = prof[i];
    const nm = st ? nameOf(i) : '';
    setText(p.eq.legend, 'Еквалайзер: ' + nm);
    setText(p.bal.legend, 'Баланс: ' + nm);

    p.eq.fs.disabled = !c.eq;
    if (c.eq) {
      p.eq.rows.forEach((r) => {
        setAttr(r.input, 'min', c.eq.minDb);            // min / max / step — ПЕРЕД value
        setAttr(r.input, 'max', c.eq.maxDb);
        setAttr(r.input, 'step', c.eq.stepDb);
        const f = c.eq.freqHz ? c.eq.freqHz[r.b] : null;
        const label = isNum(f) ? fmtFreq(f) : 'Смуга ' + (r.b + 1);
        setText(r.title, label);
        setAttr(r.input, 'aria-label', 'Еквалайзер ' + label);
        if (!r.ctl.held()) { r.input.value = c.eq.gainsDb[r.b]; setText(r.val, fmtDb(c.eq.gainsDb[r.b])); }
      });
    }

    p.bal.fs.disabled = c.bal === null || balLocked(i);
    setAttr(p.bal.input, 'min', CONFIG.balanceMinDb);
    setAttr(p.bal.input, 'max', CONFIG.balanceMaxDb);
    setAttr(p.bal.input, 'step', CONFIG.balanceStepDb);
    if (c.bal !== null && !p.bal.ctl.held()) { p.bal.input.value = c.bal; setText(p.bal.val, fmtDb(c.bal)); }
  }

  function visibleInputs() {
    const list = [];
    for (let i = 0; i < INPUTS; i++) {
      const hw = !((st && st.inputs) || []).some((x) => x.index === i && x.available === false);
      if (hw && isInputEnabled(i)) list.push(i);        // вимкнені входи ховаємо (як на «Головній», P39b)
    }
    return list;
  }

  function pickSelection() {
    const vis = visibleInputs();
    if (selected === null || !vis.includes(selected)) {
      selected = vis.includes(st.input) ? st.input : (vis.length ? vis[0] : 0);
    }
  }

  function renderTabs() {
    const vis = visibleInputs();
    const sig = JSON.stringify(vis.map((i) => [i, nameOf(i)]));
    if (sig !== tabsSig) {
      tabsSig = sig;
      ui.tabs.replaceChildren(...vis.map((i) => el('button', { type: 'button', role: 'radio', 'data-i': i }, nameOf(i))));
    }
    [...ui.tabs.children].forEach((b) => {
      const i = Number(b.dataset.i);
      const active = st.input === i;
      setAttr(b, 'aria-checked', i === selected);
      setAttr(b, 'data-active', active);
      setAttr(b, 'aria-label', nameOf(i) + (active ? ', зараз грає' : ''));
      setAttr(b, 'title', active ? 'Активний вхід пристрою' : '');
    });
  }

  function renderBanners(lock) {
    setBanner('ota', lock === 'ota' ? 'Оновлення прошивки: ' + (st.otaProgress || 0) + '%. Керування вимкнено.' : null,
      'warn', { progress: st && st.otaProgress });
    setBanner('irlearn', st && st.mode === 'IrLearn' ? 'Іде навчання кнопок пульта: баланс активного входу тимчасово недоступний.' : null, 'warn');
  }

  function render() {
    if (!st) { ui.grp.disabled = true; return; }
    const lock = lockKind();
    renderBanners(lock);
    renderWifi(ui.wifi, st.wifi);
    ui.grp.disabled = lock !== null;
    pickSelection();
    renderTabs();

    panels.forEach((p, i) => {
      p.eq.box.hidden = i !== selected;
      p.bal.box.hidden = i !== selected;
      renderPanel(i);
    });

    const sel = selected, name = nameOf(sel);
    setText(ui.scope, sel === st.input
      ? 'Зараз грає цей вхід: зміни чути одразу. Значення зберігаються в профілі входу «' + name + '».'
      : 'Редагується лише профіль входу «' + name + '»: звук не зміниться, доки ви не перемкнетесь на цей вхід. Зараз грає «' + nameOf(st.input) + '».');

    let note = null;
    if (balLocked(sel)) {
      note = isStandby()
        ? 'Пристрій у режимі очікування: баланс активного входу зараз змінити не можна. Баланс інших входів — можна.'
        : 'Іде навчання пульта: баланс активного входу тимчасово недоступний.';
    }
    setText(ui.balNote, note || '');
    ui.balNote.hidden = !note;

    ui.reset.disabled = resetBusy || !prof[sel].eq || prof[sel].bal === null;
  }

  /* ---------- Дії ---------- */
  ui.tabs.addEventListener('click', (ev) => {
    const b = ev.target.closest('button[data-i]');
    if (!b || !st) return;
    const i = Number(b.dataset.i);
    if (i === selected) return;
    selected = i;
    setErr(ui.eqErr, null); setErr(ui.balErr, null);
    render();
    poller.now();                       // одразу перечитати профіль вибраного входу
  });

  ui.reset.addEventListener('click', async () => {
    const i = selected;
    if (i === null || !st || resetBusy) return;
    const ok = await confirmDialog('Скинути еквалайзер (усі смуги 0 дБ) і баланс (0) входу «' + nameOf(i) +
      '»? Гучність і інші входи не зміняться.', { okText: 'Скинути', danger: true });
    if (!ok) return;
    resetBusy = true; render();
    panels[i].eq.rows.forEach((r) => r.ctl.release());
    panels[i].bal.ctl.release();
    const fails = [];
    try { applyEq(i, await api.setEq({ gainsDb: new Array(BANDS).fill(0) }, i)); } catch (e) { fails.push('еквалайзер — ' + errorText(e)); }
    try { applyBal(i, await api.setBalance(CONFIG.balanceNeutralDb, i)); } catch (e) { fails.push('баланс — ' + errorText(e)); }
    if (fails.length) toast('Не все скинуто: ' + fails.join(' '), 'error');
    else toast('Вхід «' + nameOf(i) + '»: еквалайзер і баланс скинуто.', 'ok');
    resetBusy = false; render();
    poller.now();
  });

  /* ---------- Опитування ---------- */
  async function refresh() {
    st = await api.get('/api/status');
    syncInputs(st.input);
    pickSelection();
    render();
    await loadProfile(selected);        // повзунок, який зараз тягнуть (held), не перезаписується
    render();
  }
  const poller = createPoller(refresh, { intervalMs: CONFIG.audioPollMs });

  initShell('audio');
  onConnectionChange(render);
  onInputsChange(() => render());
  render();
  poller.start();
})();
