'use strict';
/* Сторінка «Станції» (Prompt 20, 25b). Залежить від common.js і nav.js.
   Індекси станцій ПОЗИЦІЙНІ: після будь-якої зміни списку перечитуємо список і стан. */
(() => {
  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), grp: g('grp'), count: g('count'), fullHint: g('full-hint'), search: g('search'),
    list: g('list'), empty: g('empty'), emptyText: g('empty-text'), emptyActions: g('empty-actions'),
    file: g('file'), exp: g('btn-export'),
  };
  const addBtns = [...document.querySelectorAll('[data-act="add"]')];
  const CTRL = /[\u0000-\u001f\u007f]/;
  const IMPORT_TYPES = { json: 'application/json', m3u: 'audio/x-mpegurl', m3u8: 'audio/x-mpegurl', pls: 'audio/x-scpls' };

  let st = null;                 // останній /api/status
  let stations = [];             // останній /api/stations
  let rows = [];                 // [{li, index, name, url, level, key, up, dn, cur}]
  let byIndex = new Map();
  let shown = 0;                 // скільки рядків видно після фільтра
  let filter = '';
  let loaded = false;
  let loadError = false;
  let loading = false;
  let again = false;
  let nextTry = 0;
  let countTried = -1;
  let busy = false;

  /* ---------- Стан ---------- */
  const isRadio = () => !!st && st.input === 0;
  const maxCount = () => (st && st.station && typeof st.station.max === 'number' ? st.station.max : null);
  const isFull = () => maxCount() !== null && stations.length >= maxCount();
  const radioName = () => (st && st.inputs && st.inputs[0] && st.inputs[0].name) || 'WiFi Radio';

  function lockKind() {
    if (!st) return 'loading';
    if (connection.offline) return 'offline';
    if (st.mode === 'OtaUpdate') return 'ota';
    if (st.mode === 'IrLearn') return 'irlearn';
    if (st.mode === 'WifiSetup') return 'wifisetup';
    return null;
  }

  /* ---------- Рендер ---------- */
  function renderBanners(lock) {
    setBanner('ota', lock === 'ota' ? 'Оновлення прошивки: ' + (st.otaProgress || 0) + '%. Редагування вимкнено.' : null,
      'warn', { progress: st && st.otaProgress });
    setBanner('irlearn', lock === 'irlearn' ? 'Іде навчання кнопок пульта. Редагування вимкнено.' : null, 'warn');
    setBanner('wifisetup', lock === 'wifisetup' ? 'Пристрій у режимі налаштування Wi-Fi. Редагування вимкнено.' : null, 'warn');
  }

  function updateRows() {
    const cur = isRadio() && st.station ? st.station.index : -1;
    const last = rows.length - 1;
    rows.forEach((r, i) => {
      const isCur = r.index === cur;
      if (isCur !== !!r.cur) {
        r.cur = isCur;
        if (isCur) r.li.setAttribute('aria-current', 'true'); else r.li.removeAttribute('aria-current');
      }
      r.up.disabled = filter !== '' || i === 0;      // під фільтром сусіди приховані - позиції оманливі
      r.dn.disabled = filter !== '' || i === last;
    });
  }

  function renderEmpty() {
    let text = null;
    let actions = false;
    if (!loaded) text = loadError ? 'Не вдалося завантажити список. Повторюємо спроби…' : 'Завантаження…';
    else if (stations.length === 0) {
      text = 'Список порожній. Додайте станцію вручну або імпортуйте плейлист (.json, .m3u, .m3u8, .pls).';
      actions = true;
    } else if (shown === 0) text = 'Нічого не знайдено.';
    ui.empty.hidden = text === null;
    ui.emptyActions.hidden = !actions;
    if (text !== null) setText(ui.emptyText, text);
  }

  function render() {
    const lock = lockKind();
    renderBanners(lock);
    if (st) renderWifi(ui.wifi, st.wifi);
    ui.grp.disabled = lock !== null || busy;
    ui.grp.classList.toggle('is-busy', busy && lock === null);
    const n = stations.length;
    const max = maxCount();
    const full = isFull();
    setText(ui.count, loaded ? (max !== null ? n + ' / ' + max : String(n)) : '');
    addBtns.forEach((b) => { b.disabled = full; });
    ui.fullHint.hidden = !full;
    if (full) setText(ui.fullHint, 'Список заповнений (' + max + '). Видаліть станцію, щоб додати нову.');
    ui.exp.disabled = n === 0;
    ui.search.hidden = n <= CONFIG.stationSearchThreshold;
    renderEmpty();
    updateRows();
  }

  function buildList() {
    const frag = document.createDocumentFragment();
    byIndex = new Map();
    rows = stations.map((s, pos) => {
      const index = typeof s.index === 'number' ? s.index : pos;
      const name = String(s.name || '');
      const url = String(s.url || '');
      const level = Number.isInteger(s.levelDb) ? s.levelDb : null;   // стара відповідь без levelDb - без бейджа
      const btn = (act, ic, label) => el('button', {
        type: 'button', class: 'btn btn-icon', 'data-act': act, title: label, 'aria-label': label + ' «' + name + '»',
      }, icon(ic));
      const li = el('li', { class: 'station', 'data-i': index },
        el('div', { class: 'station-main' },
          el('span', { class: 'idx num' }, index + 1),
          el('div', { class: 'station-text' },
            el('div', { class: 'station-name' }, name),
            el('div', { class: 'station-url muted', title: url }, url)),
          level === null ? null : el('span', { class: 'badge station-level num', title: 'Рівень виходу декодера' }, fmtSigned(level) + ' дБ')),
        el('div', { class: 'station-actions' },
          btn('play', 'play', 'Грати'), btn('up', 'arrow-up', 'Вище'), btn('down', 'arrow-down', 'Нижче'),
          btn('move', 'move', 'На позицію…'), btn('edit', 'edit', 'Редагувати'), btn('del', 'trash', 'Видалити')));
      const row = {
        li, index, name, url, level, key: (name + ' ' + url).toLowerCase(), cur: false,
        up: li.querySelector('[data-act="up"]'), dn: li.querySelector('[data-act="down"]'),
      };
      byIndex.set(index, row);
      frag.append(li);
      return row;
    });
    ui.list.replaceChildren(frag);
    ui.search.value = '';
    filter = '';
    shown = rows.length;
  }

  function applyFilter() {
    filter = ui.search.value.trim().toLowerCase();
    shown = 0;
    rows.forEach((r) => {
      const hide = filter !== '' && !r.key.includes(filter);
      r.li.hidden = hide;
      if (!hide) shown++;
    });
    renderEmpty();
    updateRows();
  }

  function reveal(index) {
    const r = byIndex.get(index);
    if (r) r.li.scrollIntoView({ block: 'nearest' });
  }

  /* ---------- Завантаження ---------- */
  async function reload(manual) {
    if (loading) { again = true; return; }
    loading = true;
    try {
      const list = await api.get('/api/stations', CONFIG.stationsTimeoutMs);
      stations = Array.isArray(list) ? list : [];
      loaded = true; loadError = false;
      buildList();
      if (manual) toast('Список станцій оновлено', 'ok');
    } catch (e) {
      const first = !loadError;
      loadError = true;
      nextTry = Date.now() + CONFIG.stationsRetryMs;
      if (manual || (first && !loaded)) toast('Список станцій: ' + errorText(e), 'error');
    }
    try { st = await api.get('/api/status'); } catch (_) { /* пулер відновить */ }
    loading = false;
    render();
    if (again) { again = false; await reload(); }
  }

  function maybeLoad() {
    if (loading || Date.now() < nextTry) return;
    const count = st.station ? st.station.count : 0;
    if (!loaded || loadError || (count !== stations.length && count !== countTried)) {
      countTried = count;
      reload();
    }
  }

  async function refreshStatus() {
    st = await api.get('/api/status');
    render();
    maybeLoad();
  }
  const poller = createPoller(refreshStatus, { intervalMs: CONFIG.stationsPollMs });

  /* ---------- Виконання дій ---------- */
  async function guarded(fn) {
    if (busy) return;
    busy = true; render();
    try { await fn(); } catch (e) { toast(errorText(e), 'error'); }
    busy = false; render();
  }
  /* Зміна списку: після неї (навіть невдалої) завжди перечитуємо список і стан. */
  function write(fn) {
    return guarded(async () => { try { await fn(); } finally { await reload(); } });
  }

  /* ---------- Форма (додавання, редагування, переміщення) ---------- */
  function setErr(fld, text) {
    fld.err.hidden = !text;
    setText(fld.err, text || '');
    if (text) fld.input.setAttribute('aria-invalid', 'true'); else fld.input.removeAttribute('aria-invalid');
  }

  /* openForm({title, submit, fields:[{key,label,value,bytes?,type?,inputmode?,min?,max?}],
     validate(vals)->{key:msg}, save(vals)->Promise, level?:{value, apply?(db)->Promise}}) -> Promise<boolean>
     (true = збережено). level (P25b) додає поле «Рівень» -> vals.levelDb; level.apply (лише редагування)
     додає «Застосувати»: зберігає рівень без закриття діалогу. */
  function openForm(o) {
    return new Promise((resolve) => {
      const f = {};
      const nodes = o.fields.map((fd) => {
        const id = 'f-' + fd.key;
        const input = el('input', {
          class: 'input', id, type: fd.type || 'text', value: fd.value, autocomplete: 'off', spellcheck: 'false',
          inputmode: fd.inputmode, min: fd.min, max: fd.max, 'aria-describedby': id + '-err',
        });
        const err = el('div', { class: 'field-error', id: id + '-err', role: 'alert', hidden: true });
        const cnt = fd.bytes ? el('span', { class: 'hint num' }) : null;
        f[fd.key] = { input, err };
        const upd = () => { if (cnt) setText(cnt, byteLength(input.value) + ' / ' + fd.bytes + ' байт'); };
        input.addEventListener('input', () => { upd(); setErr(f[fd.key], ''); });
        upd();
        return el('div', { class: 'field' }, el('label', { for: id }, fd.label), input, err, cnt);
      });
      let sending = false;
      let applying = false;
      let badge = null;
      let applyBtn = null;
      let hideTimer = null;
      const cancel = el('button', { class: 'btn', type: 'button', onclick: () => { if (!sending && !applying) dlg.close('cancel'); } }, 'Скасувати');
      const okBtn = el('button', { class: 'btn btn-primary', type: 'submit' }, o.submit);
      const sync = () => {
        const b = sending || applying;
        okBtn.disabled = b; cancel.disabled = b;
        if (applyBtn) applyBtn.disabled = b;
      };

      if (o.level) {
        const lo = CONFIG.stationLevelMinDb, hi = CONFIG.stationLevelMaxDb;
        const input = el('input', {
          class: 'slider', type: 'range', id: 'f-levelDb', min: lo, max: hi, step: 1,
          value: clamp(o.level.value, lo, hi), 'aria-describedby': 'f-levelDb-hint f-levelDb-err',
        });
        const out = el('output', { class: 'level-val num', for: 'f-levelDb' });
        const err = el('div', { class: 'field-error', id: 'f-levelDb-err', role: 'alert', hidden: true });
        f.levelDb = { input, err };
        const show = () => setText(out, fmtSigned(Number(input.value)) + ' дБ');
        const stepBtn = (ic, label, d) => el('button', {
          class: 'btn btn-icon', type: 'button', 'aria-label': label,
          onclick: () => { input.value = clamp(Number(input.value) + d, lo, hi); input.dispatchEvent(new Event('input')); },
        }, icon(ic));
        input.addEventListener('input', () => { show(); setErr(f.levelDb, ''); if (badge) badge.hidden = true; });
        show();
        const extra = [];
        if (o.level.apply) {
          badge = el('span', { class: 'badge badge-ok', role: 'status', hidden: true }, 'Застосовано');
          applyBtn = el('button', {
            class: 'btn', type: 'button',
            onclick: async () => {
              if (sending || applying) return;
              applying = true; sync(); setErr(f.levelDb, '');
              try {
                await o.level.apply(Number(input.value));
                badge.hidden = false;
                clearTimeout(hideTimer);
                hideTimer = setTimeout(() => { badge.hidden = true; }, CONFIG.savedMs);
              } catch (e) { setErr(f.levelDb, errorText(e)); }
              applying = false; sync();
            },
          }, 'Застосувати');
          extra.push(el('div', { class: 'row-flex' }, applyBtn, badge),
            el('p', { class: 'hint' }, 'Рівень звучить одразу лише якщо ця станція зараз грає, інакше застосується при її запуску. ' +
              '«Застосувати» зберігає лише рівень; назву й адресу зберігає «Зберегти».'));
        }
        nodes.push(el('div', { class: 'field' },
          el('label', { for: 'f-levelDb' }, 'Рівень, дБ'),
          el('div', { class: 'param-row' }, stepBtn('minus', 'Рівень: тихіше', -1), input, stepBtn('plus', 'Рівень: гучніше', 1), out),
          el('p', { class: 'hint', id: 'f-levelDb-hint' },
            'Нижче значення — тихіше. Допомагає, якщо звук спотворений або станція гучніша за інші.'),
          err, extra));
      }

      const form = el('form', { novalidate: true },
        el('h2', null, o.title), el('div', { class: 'form' }, nodes), el('div', { class: 'dialog-actions' }, cancel, okBtn));
      const dlg = el('dialog', { class: 'dialog dialog-form' }, form);

      form.addEventListener('submit', async (ev) => {
        ev.preventDefault();
        if (sending || applying) return;
        const vals = {};
        o.fields.forEach((fd) => { vals[fd.key] = f[fd.key].input.value; });
        if (o.level) { vals.levelDb = Number(f.levelDb.input.value); setErr(f.levelDb, ''); }
        const errs = o.validate(vals);
        const bad = o.fields.filter((fd) => errs[fd.key]);
        o.fields.forEach((fd) => setErr(f[fd.key], errs[fd.key] || ''));
        if (bad.length) { f[bad[0].key].input.focus(); return; }
        sending = true; sync();
        try {
          await o.save(vals);
          dlg.close('saved');
        } catch (e) {
          const fld = e instanceof ApiError && e.data ? f[e.data.field] : null;   // помилка конкретного поля
          if (fld) { setErr(fld, errorText(e)); fld.input.focus(); } else toast(errorText(e), 'error');
          sending = false; sync();
        }
      });
      dlg.addEventListener('cancel', (ev) => { if (sending || applying) ev.preventDefault(); });
      dlg.addEventListener('close', () => { clearTimeout(hideTimer); dlg.remove(); resolve(dlg.returnValue === 'saved'); });
      document.body.append(dlg);
      dlg.showModal();
    });
  }

  /* Дзеркало серверної валідації (байти UTF-8, а не символи). Обрізає пробіли. */
  function validateStation(v) {
    const errs = {};
    v.name = v.name.trim(); v.url = v.url.trim();
    if (!v.name) errs.name = 'Введіть назву.';
    else if (CTRL.test(v.name)) errs.name = 'Назва містить керівні символи.';
    else if (byteLength(v.name) > CONFIG.stationNameMaxBytes) {
      errs.name = 'Назва задовга: максимум ' + CONFIG.stationNameMaxBytes + ' байт (кирилична літера = 2 байти).';
    }
    if (!v.url) errs.url = 'Введіть адресу потоку.';
    else if (!/^https?:\/\//.test(v.url)) errs.url = 'Адреса має починатися з http:// або https://';
    else if (CTRL.test(v.url)) errs.url = 'Адреса містить керівні символи.';
    else if (byteLength(v.url) > CONFIG.stationUrlMaxBytes) errs.url = 'Адреса задовга: максимум ' + CONFIG.stationUrlMaxBytes + ' байт.';
    return errs;
  }
  const stationFields = (name, url) => [
    { key: 'name', label: 'Назва', value: name, bytes: CONFIG.stationNameMaxBytes },
    { key: 'url', label: 'Адреса потоку (URL)', value: url, bytes: CONFIG.stationUrlMaxBytes, inputmode: 'url' },
  ];

  /* ---------- Дії ---------- */
  async function addStation() {
    if (isFull()) return;
    let newIndex = null;
    const ok = await openForm({
      title: 'Нова станція', submit: 'Додати', fields: stationFields('', ''), validate: validateStation,
      level: { value: CONFIG.stationLevelDefaultDb },
      save: async (v) => { const r = await api.post('/api/stations', v, CONFIG.stationsWriteTimeoutMs); newIndex = r.index; },
    });
    if (!ok) return;
    toast('Станцію додано', 'ok');
    await write(async () => {});
    if (newIndex !== null) reveal(newIndex);
  }

  async function editStation(r) {
    const ok = await openForm({
      title: 'Редагувати станцію', submit: 'Зберегти', fields: stationFields(r.name, r.url), validate: validateStation,
      level: {
        value: r.level === null ? CONFIG.stationLevelDefaultDb : r.level,
        // «Застосувати»: PUT зі збереженими name / url рядка (не зі значеннями форми) і рівнем форми; список - у тлі
        apply: async (db) => {
          await api.put('/api/stations/' + r.index, { name: r.name, url: r.url, levelDb: db }, CONFIG.stationsWriteTimeoutMs);
          reload();
        },
      },
      save: (v) => api.put('/api/stations/' + r.index, v, CONFIG.stationsWriteTimeoutMs),
    });
    if (!ok) return;
    toast('Зміни збережено', 'ok');
    await write(async () => {});
    reveal(r.index);
  }

  async function deleteStation(r) {
    const cur = isRadio() && st.station && st.station.index === r.index;
    const text = 'Видалити станцію «' + r.name + '»?' +
      (cur ? ' Це поточна станція: потік не перерветься, але позначка «поточна» може показати іншу станцію.' : '');
    if (!(await confirmDialog(text, { okText: 'Видалити', danger: true }))) return;
    await write(async () => {
      await api.del('/api/stations/' + r.index, CONFIG.stationsWriteTimeoutMs);
      toast('Станцію видалено', 'ok');
    });
  }

  async function moveStation(from, to) {
    await write(() => api.post('/api/stations/move', { from, to }, CONFIG.stationsWriteTimeoutMs));
    reveal(to);
  }

  async function moveToPosition(r) {
    const n = stations.length;
    let target = r.index;
    const ok = await openForm({
      title: 'Перемістити «' + r.name + '»', submit: 'Перемістити',
      fields: [{ key: 'pos', label: 'На позицію (1…' + n + ')', value: String(r.index + 1), type: 'number', inputmode: 'numeric', min: 1, max: n }],
      validate: (v) => {
        const x = Number(v.pos);
        return /^\d+$/.test(v.pos.trim()) && x >= 1 && x <= n ? {} : { pos: 'Введіть ціле число від 1 до ' + n + '.' };
      },
      save: async (v) => {
        target = Number(v.pos) - 1;
        if (target !== r.index) await api.post('/api/stations/move', { from: r.index, to: target }, CONFIG.stationsWriteTimeoutMs);
      },
    });
    if (!ok || target === r.index) return;
    await write(async () => {});
    reveal(target);
  }

  async function play(r) {
    const radio = isRadio();
    const body = { index: r.index };
    if (!radio) body.switchInput = true;        // контракт §4.7: без прапорця поза Radio буде 409
    await guarded(async () => {
      await api.post('/api/player/station', body);
      if (!radio) toast('Перемкнено на вхід «' + radioName() + '»', 'ok');
      if (st) { st.input = 0; if (st.station) st.station.index = r.index; }
    });
    poller.now();
  }

  function exportList() {
    return guarded(async () => {
      const blob = await api.blob('/api/stations/export', CONFIG.stationsTimeoutMs);
      downloadBlob(blob, 'stations-' + fileStamp() + '.json');
      toast('Експорт завершено', 'ok');
    });
  }

  async function importFile(file, fmt, type) {
    const before = stations.length;
    setBanner('import-result', null);
    setBanner('importing', 'Імпорт «' + file.name + '»… Не закривайте сторінку.', 'info');
    await guarded(async () => {
      try {
        const r = await api.send('POST', '/api/stations/import?format=' + fmt, file, type, CONFIG.importTimeoutMs);
        setBanner('import-result', 'Імпорт завершено: у списку ' + r.count + ' станцій (було ' + before + '). Попередній список замінено.', 'info');
      } catch (e) {
        const lost = e instanceof ApiError && (e.code === 'timeout' || e.code === 'network');
        const code = e instanceof ApiError && e.reason ? ' (код: ' + e.reason + ')' : '';
        setBanner('import-result', 'Імпорт не вдався: ' + errorText(e) + code + ' ' +
          (lost ? 'Відповідь не отримано - перевірте список нижче.' : 'Попередній список не змінено.'), 'error');
        toast(errorText(e), 'error');
      } finally {
        setBanner('importing', null);
        await reload();
      }
    });
  }

  ui.file.addEventListener('change', async () => {
    const file = ui.file.files[0];
    ui.file.value = '';
    if (!file) return;
    const ext = (file.name.split('.').pop() || '').toLowerCase();
    const type = IMPORT_TYPES[ext];
    if (!type) { toast('Непідтримуваний тип файлу. Потрібні .json, .m3u, .m3u8 або .pls.', 'error'); return; }
    if (file.size === 0) { toast(REASONS.empty, 'error'); return; }
    if (file.size > CONFIG.importMaxBytes) {
      toast('Файл завеликий: ' + formatBytes(file.size) + ' (максимум ' + formatBytes(CONFIG.importMaxBytes) + ').', 'error');
      return;
    }
    if (stations.length > 0 && !(await confirmDialog(
      'Імпорт замінить увесь список (' + stations.length + ' станцій) вмістом файлу «' + file.name + '». Скасувати це буде неможливо. Продовжити?',
      { okText: 'Замінити список', danger: true }))) return;
    importFile(file, ext === 'm3u8' ? 'm3u' : ext, type);
  });

  const ACTIONS = {
    add: addStation,
    import: () => ui.file.click(),
    export: exportList,
    refresh: () => guarded(() => reload(true)),
    play,
    up: (r) => moveStation(r.index, r.index - 1),
    down: (r) => moveStation(r.index, r.index + 1),
    move: moveToPosition,
    edit: editStation,
    del: deleteStation,
  };
  document.querySelector('main').addEventListener('click', (ev) => {
    const b = ev.target.closest('button[data-act]');
    if (!b || b.disabled || busy) return;
    const li = b.closest('li.station');
    const row = li ? byIndex.get(Number(li.dataset.i)) : null;
    const fn = ACTIONS[b.dataset.act];
    if (fn && (row || !li)) fn(row);
  });
  ui.search.addEventListener('input', debounce(applyFilter, CONFIG.searchDebounceMs));

  setText(g('import-hint'), 'JSON зберігає рівні станцій; M3U / PLS отримують значення за замовчуванням (' +
    fmtSigned(CONFIG.stationLevelDefaultDb) + ' дБ).');
  initShell('stations');
  onConnectionChange(render);
  render();
  poller.start();
})();
