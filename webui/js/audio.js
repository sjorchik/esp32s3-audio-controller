'use strict';
/* Сторінка «Аудіо» (Prompt 21). Залежить від common.js і nav.js. */
(() => {
  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), standby: g('standby'), wake: g('btn-wake'), noProc: g('no-proc'), main: g('main'),
    grpVolume: g('grp-volume'), grpGain: g('grp-gain'), grpInput: g('grp-input'), grpTone: g('grp-tone'),
    chip: g('chip'), vol: g('vol'), volOut: g('vol-out'), volDn: g('vol-dn'), volUp: g('vol-up'), mute: g('btn-mute'),
    gainName: g('gain-input'), gainSeg: g('gain-seg'), gainDb: g('gain-db'), inputs: g('inputs'),
    params: g('params'), loudBox: g('loud-box'), loud: g('loud'), loudNo: g('loud-no'), reset: g('btn-reset'),
  };
  const CHIP = { Tda7318: 'TDA7318', Pt2313l: 'PT2313L' };

  let st = null;          // останній /api/status
  let gainSig = '';
  let inputsSig = '';
  let optUntil = 0;       // до цього часу опитування не перезаписує оптимістичні gain / input

  const caps = () => (st && st.processor && st.processor.ready ? st.processor.capabilities : null);
  const isStandby = () => !!st && (st.standby === true || st.mode === 'Standby');
  function lockKind() {
    if (!st) return 'loading';
    if (connection.offline) return 'offline';
    if (st.mode === 'OtaUpdate') return 'ota';
    if (st.mode === 'IrLearn') return 'irlearn';
    if (st.mode === 'WifiSetup') return 'wifisetup';
    return null;
  }
  const dbText = (n, step) => {
    const d = Math.round(n * step * 10) / 10;
    return (d > 0 ? '+' : '') + String(d).replace('.', ',') + ' дБ';
  };
  const sideText = (v) => (v < 0 ? 'Л ' + (-v) : v > 0 ? 'П ' + v : 'Центр');
  const iconBtn = (name, label, fn) => {
    const b = el('button', { class: 'btn btn-icon', type: 'button', 'aria-label': label, onclick: fn });
    setIcon(b, name);
    return b;
  };

  /* ---------- Команди ---------- */
  async function command(path, body) {
    try {
      const r = await api.post(path, body);
      if (st && r.state) {
        st.mode = r.state.mode; st.standby = r.state.standby; st.input = r.state.input; st.mute = r.state.mute;
        if (!volCtl.held()) st.volume = r.state.volume;
      }
      render();
      poller.now();
      return r;
    } catch (e) {
      toast(errorText(e), 'error');
      render();
      poller.now();
      return null;
    }
  }

  /* ---------- Гучність ---------- */
  const volCtl = createSlider(ui.vol, {
    send: async (v) => { const r = await api.post('/api/volume', { value: v }); if (r.state) st.volume = r.state.volume; },
    onInput: (v) => setText(ui.volOut, String(v)),
    onFail: () => render(),
  });
  ui.volDn.addEventListener('click', () => command('/api/volume', { step: -CONFIG.volumeButtonStep }));
  ui.volUp.addEventListener('click', () => command('/api/volume', { step: CONFIG.volumeButtonStep }));
  ui.mute.addEventListener('click', async () => {
    if (!st) return;
    const want = !st.mute;
    st.mute = want; render();
    if (!(await command('/api/mute', { mute: want }))) { st.mute = !want; render(); }
  });
  ui.wake.addEventListener('click', () => command('/api/power', { state: 'on' }));

  /* ---------- Бас, дискант, баланс ---------- */
  const DEFS = [
    { key: 'bass', title: 'Бас', cap: 'bass', fmt: fmtSigned, n: 'tone', zero: '0', lo: 'toneMin', hi: 'toneMax' },
    { key: 'treble', title: 'Дискант', cap: 'treble', fmt: fmtSigned, n: 'tone', zero: '0', lo: 'toneMin', hi: 'toneMax' },
    { key: 'balance', title: 'Баланс', cap: 'balance', fmt: sideText, n: 'balance', zero: 'Центр', lo: 'balanceMin', hi: 'balanceMax' },
  ];
  const neutralOf = (d) => clamp(CONFIG.audioNeutral[d.n], caps()[d.lo], caps()[d.hi]);

  const params = DEFS.map((d) => {
    const input = el('input', { class: 'slider slider-c', type: 'range', step: 1, value: 0, 'aria-label': d.title });
    const val = el('output', { class: 'param-val num' });
    const p = { d, input, val };
    p.ctl = createSlider(input, {
      send: async (v) => { await api.post('/api/settings', { [d.key]: v }); st[d.key] = v; },
      onInput: (v) => setText(val, d.fmt(v)),
      onFail: () => render(),
    });
    const set = (v) => {
      v = clamp(v, Number(input.min), Number(input.max));
      input.value = v; setText(val, d.fmt(v));
      p.ctl.send(v);
    };
    p.row = el('div', { class: 'param' },
      el('div', { class: 'param-head' }, el('span', null, d.title), val),
      el('div', { class: 'param-row' },
        iconBtn('minus', d.title + ': менше', () => set(Number(input.value) - 1)),
        input,
        iconBtn('plus', d.title + ': більше', () => set(Number(input.value) + 1)),
        el('button', { class: 'btn btn-zero', type: 'button', onclick: () => set(neutralOf(d)) }, d.zero)));
    ui.params.append(p.row);
    return p;
  });

  ui.loud.addEventListener('change', async () => {
    const want = ui.loud.checked;
    try {
      await api.post('/api/settings', { loudness: want });
      st.loudness = want;
    } catch (e) {
      toast(errorText(e), 'error');
      ui.loud.checked = !want;
    }
    poller.now();
  });

  ui.reset.addEventListener('click', async () => {
    const c = caps();
    if (!st || !c) return;
    const ok = await confirmDialog('Повернути бас, дискант і баланс до нейтральних значень, а тонкомпенсацію — до значення за замовчуванням? Підсилення не зміниться.',
      { okText: 'Скинути', danger: true });
    if (!ok) return;
    const body = {};
    params.forEach((p) => { if (c[p.d.cap]) body[p.d.key] = neutralOf(p.d); });
    if (c.loudness) body.loudness = CONFIG.audioNeutral.loudness;
    if (!Object.keys(body).length) return;
    try {
      await api.post('/api/settings', body);
      Object.assign(st, body);
      params.forEach((p) => p.ctl.release());
      toast('Тембр скинуто', 'ok');
    } catch (e) {
      toast(errorText(e), 'error');
    }
    render();
    poller.now();
  });

  /* ---------- Підсилення та вхід ---------- */
  ui.gainSeg.addEventListener('click', async (ev) => {
    const b = ev.target.closest('button[data-n]');
    if (!b || !st) return;
    const n = Number(b.dataset.n);
    if (n === st.gain) return;
    const prev = st.gain;
    st.gain = n; optUntil = Date.now() + CONFIG.volumeHoldMs; render();
    if (!(await command('/api/gain', { value: n }))) { optUntil = 0; st.gain = prev; render(); }
  });
  ui.inputs.addEventListener('click', async (ev) => {
    const b = ev.target.closest('button[data-i]');
    if (!b || !st) return;
    const i = Number(b.dataset.i);
    if (i === st.input) return;
    const prev = st.input;
    st.input = i; optUntil = Date.now() + CONFIG.volumeHoldMs; render();
    if (!(await command('/api/input', { index: i }))) { optUntil = 0; st.input = prev; render(); }
  });

  /* ---------- Рендер ---------- */
  function renderBanners(lock) {
    setBanner('ota', lock === 'ota' ? 'Оновлення прошивки: ' + (st.otaProgress || 0) + '%. Керування вимкнено.' : null,
      'warn', { progress: st && st.otaProgress });
    setBanner('irlearn', lock === 'irlearn' ? 'Іде навчання кнопок пульта. Керування вимкнено.' : null, 'warn');
    setBanner('wifisetup', lock === 'wifisetup' ? 'Пристрій у режимі налаштування Wi-Fi. Керування вимкнено.' : null, 'warn');
  }

  function renderGain(c) {
    const step = CONFIG.gainStepDb[st.processor.type] || 0;
    const sig = [c.gainMin, c.gainMax, step].join();
    if (sig !== gainSig) {
      gainSig = sig;
      const btns = [];
      for (let n = c.gainMin; n <= c.gainMax; n++) {
        btns.push(el('button', { type: 'button', role: 'radio', 'data-n': n }, step ? n + ' · ' + dbText(n, step) : String(n)));
      }
      ui.gainSeg.replaceChildren(...btns);
    }
    [...ui.gainSeg.children].forEach((b) => setAttr(b, 'aria-checked', Number(b.dataset.n) === st.gain));
    setText(ui.gainName, st.inputName || '');
    setText(ui.gainDb, step ? '≈ ' + dbText(st.gain, step) : '');
  }

  function renderInputs(c) {
    const list = (st.inputs || []).filter((i) => i.available && i.index < c.inputCount);
    const sig = JSON.stringify(list.map((i) => [i.index, i.name]));
    if (sig !== inputsSig) {
      inputsSig = sig;
      ui.inputs.replaceChildren(...list.map((i) => el('button', { type: 'button', role: 'radio', 'data-i': i.index }, i.name)));
    }
    [...ui.inputs.children].forEach((b) => setAttr(b, 'aria-checked', Number(b.dataset.i) === st.input));
  }

  function render() {
    if (!st) {
      [ui.grpVolume, ui.grpGain, ui.grpInput, ui.grpTone].forEach((n) => { n.disabled = true; });
      return;
    }
    const lock = lockKind();
    const standby = isStandby();
    const c = caps();
    renderBanners(lock);
    renderWifi(ui.wifi, st.wifi);
    ui.standby.hidden = !standby;
    ui.wake.disabled = lock !== null;
    ui.noProc.hidden = !!c;
    ui.main.hidden = !c;
    if (!c) return;

    const off = lock !== null || standby;      // контракт: у standby лише /api/power; тембр через /api/settings не гейтиться
    ui.grpVolume.disabled = off;
    ui.grpGain.disabled = off;
    ui.grpInput.disabled = off;
    ui.grpTone.disabled = lock !== null;
    ui.grpGain.hidden = !c.inputGain;
    setText(ui.chip, CHIP[st.processor.type] || st.processor.type || '—');

    setAttr(ui.vol, 'min', c.volumeMin);
    setAttr(ui.vol, 'max', c.volumeMax);
    if (!volCtl.held()) { ui.vol.value = st.volume; setText(ui.volOut, String(st.volume)); }
    setAttr(ui.mute, 'aria-pressed', !!st.mute);
    setAttr(ui.mute, 'aria-label', st.mute ? 'Увімкнути звук' : 'Вимкнути звук');
    setIcon(ui.mute, st.mute ? 'mute' : 'volume');

    if (c.inputGain) renderGain(c);
    renderInputs(c);

    params.forEach((p) => {
      const { d } = p;
      p.row.hidden = !c[d.cap];
      if (!c[d.cap]) return;
      setAttr(p.input, 'min', c[d.lo]);
      setAttr(p.input, 'max', c[d.hi]);
      if (!p.ctl.held()) { p.input.value = st[d.key]; setText(p.val, d.fmt(st[d.key])); }
    });
    ui.loudBox.hidden = !c.loudness;
    ui.loudNo.hidden = !!c.loudness;
    ui.loud.checked = !!st.loudness;
    ui.reset.hidden = !(c.bass || c.treble || c.balance || c.loudness);
  }

  /* ---------- Опитування ---------- */
  async function refreshStatus() {
    const prev = st;
    st = await api.get('/api/status');
    if (prev && Date.now() < optUntil) { st.gain = prev.gain; st.input = prev.input; }
    render();
  }
  const poller = createPoller(refreshStatus, { intervalMs: CONFIG.pollStatusMs });

  initShell('audio');
  onConnectionChange(render);
  render();
  poller.start();
})();
