'use strict';
/* Сторінка «Wi-Fi» (Prompt 29b). Залежить від common.js і nav.js. Контракт: web_api.md §7.
   Індекси збережених мереж ПОЗИЦІЙНІ: після будь-якої зміни (і після помилки) перечитуємо GET /api/wifi.
   Пароль лише передається пристрою: не зберігається, не логується, поле очищається після відправки. */
(() => {
  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), grp: g('grp'), rows: g('cur-rows'), count: g('count'), nets: g('nets'), empty: g('nets-empty'),
    full: g('full-hint'), add: g('btn-add'), scanBtn: g('btn-scan'), scanText: g('scan-text'), found: g('scan-list'),
  };
  const CTRL = /[\u0000-\u001f\u007f]/;
  const T = CONFIG.wifiWriteTimeoutMs;
  const STATE = { Connecting: 'Підключення…', Connected: 'Підключено', ApMode: 'Точка доступу',
    ApClientConnected: 'Точка доступу (є клієнт)', Off: 'Вимкнено' };
  const GONE = ['index_out_of_range', 'invalid_index', 'not_found'];
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  /* Коди «немає такого індексу» у REASONS написані для станцій - для мереж свій текст. */
  const errText = (e) => (e instanceof ApiError && GONE.includes(e.code) ? 'Такої мережі немає в списку. Список оновлено.' : errorText(e));

  let st = null, data = null, results = null;      // /api/status, /api/wifi, результати скану
  let busy = false, scanning = false, loading = false, again = false, loadErr = false, scanWarned = false;
  let netKey = '', foundKey = '', scanMsg = '';

  const nets = () => (data && Array.isArray(data.networks) ? data.networks : []);
  const maxNets = () => (data && typeof data.max === 'number' ? data.max : 0);
  const isFull = () => maxNets() > 0 && nets().length >= maxNets();
  const lockKind = () => !st ? 'loading' : connection.offline ? 'offline' :
    st.mode === 'OtaUpdate' ? 'ota' : st.mode === 'IrLearn' ? 'irlearn' : st.mode === 'WifiSetup' ? 'wifisetup' : null;
  const lockIcon = () => el('span', { class: 'lock', title: 'Захищена', 'aria-label': 'Захищена' }, icon('lock'));
  const barsOf = (ssid, rssi) => { const b = el('span', { class: 'wifi', role: 'img' }); renderWifi(b, { connected: true, ssid, rssi }); return b; };

  /* ---------- Поточне зʼєднання ---------- */
  const cur = {};
  [['state', 'Стан'], ['ssid', 'Мережа'], ['ip', 'IP-адреса'], ['sig', 'Сигнал'], ['ch', 'Канал']].forEach(([k, label]) => {
    cur[k] = el('td');
    ui.rows.append(el('tr', null, el('td', { class: 'muted' }, label), cur[k]));
  });
  const sigBars = el('span', { class: 'wifi', role: 'img' }), sigTxt = el('span', { class: 'num' });
  cur.sig.append(el('span', { class: 'row-flex' }, sigBars, sigTxt));

  function renderCur() {
    const d = data, on = !!(d && d.connected);
    setText(cur.state, d ? STATE[d.state] || d.state : '—');
    setText(cur.ssid, (d && d.ssid) || '—');
    setText(cur.ip, (d && d.ip) || '—');
    setText(cur.ch, on && d.channel ? String(d.channel) : '—');
    setText(sigTxt, on && d.rssi ? d.rssi + ' дБм' : '—');
    renderWifi(sigBars, { connected: on, ssid: d && d.ssid, rssi: d && d.rssi });
  }

  /* ---------- Списки ---------- */
  function buildNets() {
    const list = nets(), key = JSON.stringify(list);
    if (key === netKey) return;
    netKey = key;
    const btn = (act, ic, label, n, off) => el('button', { type: 'button', class: 'btn btn-icon', 'data-act': act,
      title: label, 'aria-label': label + ' «' + n.ssid + '»', disabled: off }, icon(ic));
    ui.nets.replaceChildren(...list.map((n, i) => el('li', { class: 'station', 'data-i': n.index, 'aria-current': n.current ? 'true' : null },
      el('div', { class: 'station-main' },
        el('span', { class: 'idx num' }, i + 1),
        el('div', { class: 'station-text' },
          el('div', { class: 'station-name' }, n.ssid, n.secure && lockIcon()),
          el('div', { class: 'station-url muted' }, n.secure ? 'З паролем' : 'Відкрита мережа')),
        n.current && el('span', { class: 'badge badge-ok' }, 'Поточна')),
      el('div', { class: 'station-actions' },
        !n.current && el('button', { type: 'button', class: 'btn', 'data-act': 'connect' }, 'Підключитись'),
        btn('up', 'arrow-up', 'Вище', n, i === 0), btn('down', 'arrow-down', 'Нижче', n, i === list.length - 1),
        btn('edit', 'edit', 'Змінити пароль', n), btn('del', 'trash', 'Видалити', n)))));
  }

  const savedSsids = () => new Set(nets().map((n) => n.ssid));
  function buildFound() {
    const list = results ? results.slice().sort((a, b) => b.rssi - a.rssi) : [];
    const have = data ? savedSsids() : null, full = isFull();
    const saved = (r) => (have ? have.has(r.ssid) : !!r.saved);
    const key = list.map((r) => r.ssid + '|' + r.rssi + (saved(r) ? 's' : '')).join(',') + (full ? 'F' : '');
    if (key === foundKey) return;
    foundKey = key;
    ui.found.replaceChildren(...list.map((r, i) => {
      const s = saved(r);
      return el('li', { class: 'station', 'data-s': i },
        el('div', { class: 'station-main' },
          barsOf(r.ssid, r.rssi),
          el('div', { class: 'station-text' },
            el('div', { class: 'station-name' }, r.ssid, r.secure && lockIcon()),
            el('div', { class: 'station-url muted num' }, 'канал ' + r.channel + ' · ' + r.rssi + ' дБм')),
          s && el('span', { class: 'badge badge-ok' }, 'Збережена')),
        el('div', { class: 'station-actions' },
          el('button', { type: 'button', class: 'btn' + (s ? '' : ' btn-primary'), 'data-act': s ? 'resave' : 'new', disabled: !s && full },
            s ? 'Змінити пароль' : 'Додати')));
    }));
    ui.found._list = list;
  }

  /* ---------- Рендер ---------- */
  function render() {
    const lock = lockKind(), n = nets().length, full = isFull(), work = busy || scanning;
    setBanner('ota', lock === 'ota' ? 'Оновлення прошивки: ' + (st.otaProgress || 0) + '%. Керування вимкнено.' : null, 'warn', { progress: st && st.otaProgress });
    setBanner('irlearn', lock === 'irlearn' ? 'Іде навчання кнопок пульта. Керування вимкнено.' : null, 'warn');
    setBanner('wifisetup', lock === 'wifisetup' ? 'Пристрій налаштовує Wi-Fi. Керування вимкнено.' : null, 'warn');
    setBanner('persist', data && data.persisted === false ? 'Список ще не записано в память пристрою (повтор триває).' : null, 'warn');
    if (st) renderWifi(ui.wifi, st.wifi);
    ui.grp.disabled = lock !== null || !data || work;
    ui.grp.classList.toggle('is-busy', work && lock === null);
    renderCur();
    buildNets();
    buildFound();
    setText(ui.count, data ? n + ' / ' + maxNets() : '');
    ui.add.disabled = full;
    ui.full.hidden = !full;
    if (full) setText(ui.full, 'Список заповнений (' + maxNets() + '). Видаліть мережу, щоб додати нову; пароль наявної змінювати можна завжди.');
    ui.empty.hidden = !!data && n > 0;
    setText(ui.empty, !data ? (loadErr ? 'Не вдалося завантажити список. Повторюємо спроби…' : 'Завантаження…') :
      'Збережених мереж немає. Додайте мережу зі сканування або вручну. Без жодної мережі після перезапуску пристрій підніме власну точку доступу.');
    setText(ui.scanBtn, scanning ? 'Сканування…' : 'Сканувати');
    setText(ui.scanText, scanning ? 'Сканування триває, зазвичай 2–5 с…' : scanMsg);
  }

  /* ---------- Дані ---------- */
  async function loadWifi() {
    if (loading) { again = true; return; }                // запит уже йде: після нього перечитаємо ще раз
    loading = true;
    do {
      again = false;
      try { data = await api.get('/api/wifi'); loadErr = false; }
      catch (e) { if (!loadErr) toast('Wi-Fi: ' + errText(e), 'error'); loadErr = true; }
    } while (again);
    loading = false;
    render();
  }
  async function refreshStatus() {
    st = await api.get('/api/status');
    if (!busy && !scanning) await loadWifi(); else render();
  }
  const poller = createPoller(refreshStatus, { intervalMs: CONFIG.wifiPollMs });

  /* Дія над списком: блокує сторінку, після неї (навіть невдалої) перечитує список. */
  async function act(fn) {
    if (busy) return;
    busy = true; render();
    try { await fn(); } catch (e) { toast(errText(e), 'error'); }
    busy = false;
    await loadWifi();
  }

  /* ---------- Форма мережі ---------- */
  /* openNetForm({title, submit, ssid, fixed, mode, save(v)}) -> Promise<bool>.
     mode: 'required' (захищена), 'optional' (вручну), 'edit' (новий пароль), 'none' (відкрита). v = {ssid, password?}. */
  function openNetForm(o) {
    return new Promise((resolve) => {
      let sending = false;
      const mkErr = (id) => el('div', { class: 'field-error', id: 'w-' + id + '-err', role: 'alert', hidden: true });
      const ssid = el('input', { class: 'input', id: 'w-ssid', type: 'text', value: o.ssid, readonly: o.fixed, autocomplete: 'off',
        spellcheck: 'false', autocapitalize: 'off', 'aria-describedby': 'w-ssid-err' });
      const pass = el('input', { class: 'input', id: 'w-pass', type: 'password', autocomplete: 'new-password', spellcheck: 'false',
        autocapitalize: 'off', 'aria-describedby': 'w-pass-err w-pass-hint' });
      const E = { ssid: mkErr('ssid'), password: mkErr('pass') };
      const I = { ssid, password: pass };
      const setErr = (k, t) => { E[k].hidden = !t; setText(E[k], t || ''); setAttr(I[k], 'aria-invalid', t ? 'true' : 'false'); };
      const show = el('input', { type: 'checkbox' });
      show.addEventListener('change', () => { pass.type = show.checked ? 'text' : 'password'; });
      const open = o.mode === 'edit' ? el('input', { type: 'checkbox' }) : null;
      if (open) open.addEventListener('change', () => { pass.disabled = open.checked; if (open.checked) { pass.value = ''; setErr('password', ''); } });
      const cnt = o.fixed ? null : el('span', { class: 'hint num' });
      const upd = () => { if (cnt) setText(cnt, byteLength(ssid.value) + ' / ' + CONFIG.wifiSsidMaxBytes + ' байт'); };
      ssid.addEventListener('input', () => { upd(); setErr('ssid', ''); });
      pass.addEventListener('input', () => setErr('password', ''));
      upd();
      const range = CONFIG.wifiPassMin + '…' + CONFIG.wifiPassMax + ' символів.';
      const hint = { required: 'Мережа захищена. Пароль: ' + range, optional: 'Для відкритої мережі залиште порожнім. Інакше пароль: ' + range,
        edit: 'Новий пароль: ' + range + ' Поточний пароль не показується.', none: 'Відкрита мережа: пароль не потрібен.' }[o.mode];
      const nodes = [el('div', { class: 'field' }, el('label', { for: 'w-ssid' }, 'Назва мережі (SSID)'), ssid, cnt, E.ssid)];
      if (o.mode === 'none') nodes.push(el('p', { class: 'hint' }, hint));
      else nodes.push(el('div', { class: 'field' }, el('label', { for: 'w-pass' }, 'Пароль'), pass, E.password,
        el('label', { class: 'switch' }, show, el('span', null, 'Показати пароль')),
        open && el('label', { class: 'switch' }, open, el('span', null, 'Зробити мережу відкритою (без пароля)')),
        el('p', { class: 'hint', id: 'w-pass-hint' }, hint)));

      /* Дзеркало серверної валідації: SSID 1…32 БАЙТИ UTF-8; пароль порожній або 8…63 символи; без керівних символів. */
      function check() {
        const v = { ssid: ssid.value }, errs = {};
        if (!v.ssid) errs.ssid = 'Введіть назву мережі.';
        else if (CTRL.test(v.ssid)) errs.ssid = 'Назва містить керівні символи.';
        else if (byteLength(v.ssid) > CONFIG.wifiSsidMaxBytes) errs.ssid = 'Назва задовга: максимум ' + CONFIG.wifiSsidMaxBytes + ' байти (кирилична літера = 2 байти).';
        if (o.mode === 'none') return { v, errs };
        if (open && open.checked) { v.password = ''; return { v, errs }; }
        const p = pass.value, n = [...p].length;
        if (!p) { if (o.mode !== 'optional') errs.password = o.mode === 'edit' ? 'Введіть новий пароль або позначте «відкрита».' : 'Введіть пароль.'; }
        else if (CTRL.test(p)) errs.password = 'Пароль містить керівні символи.';
        else if (n < CONFIG.wifiPassMin || n > CONFIG.wifiPassMax) errs.password = 'Пароль: ' + range.replace('.', '') + ' (зараз ' + n + ').';
        else v.password = p;
        return { v, errs };
      }

      const cancel = el('button', { class: 'btn', type: 'button', onclick: () => { if (!sending) dlg.close('cancel'); } }, 'Скасувати');
      const okBtn = el('button', { class: 'btn btn-primary', type: 'submit' }, o.submit);
      const sync = () => { cancel.disabled = okBtn.disabled = sending; };
      const form = el('form', { novalidate: true }, el('h2', null, o.title), el('div', { class: 'form' }, nodes),
        el('div', { class: 'dialog-actions' }, cancel, okBtn));
      const dlg = el('dialog', { class: 'dialog dialog-form' }, form);
      form.addEventListener('submit', async (ev) => {
        ev.preventDefault();
        if (sending) return;
        const { v, errs } = check();
        setErr('ssid', errs.ssid || ''); setErr('password', errs.password || '');
        if (errs.ssid || errs.password) { (errs.ssid ? ssid : pass).focus(); return; }
        sending = true; sync();
        try { await o.save(v); dlg.close('saved'); }
        catch (e) {
          const k = e instanceof ApiError && e.data ? e.data.field : null;
          if (k === 'ssid' || k === 'password') { setErr(k, errText(e)); I[k].focus(); } else toast(errText(e), 'error');
          sending = false; sync();
        }
        pass.value = '';                                  // пароль не лишаємо в полі
      });
      dlg.addEventListener('cancel', (ev) => { if (sending) ev.preventDefault(); });
      dlg.addEventListener('close', () => { pass.value = ''; dlg.remove(); resolve(dlg.returnValue === 'saved'); });
      document.body.append(dlg);
      dlg.showModal();
    });
  }

  /* Нова мережа - В КІНЕЦЬ списку (без position); наявний SSID - оновлення пароля. */
  async function addNet(ssid, fixed, mode) {
    let res = null;
    const ok = await openNetForm({ title: 'Додати мережу', submit: 'Додати', ssid, fixed, mode,
      save: async (v) => { res = await postNet(v); } });
    if (!ok) return;
    toast(res.updated ? 'Мережу оновлено.' : 'Мережу додано в кінець списку (№ ' + (res.index + 1) + ').', 'ok');
    await act(async () => {});
  }
  async function editNet(n) {
    const ok = await openNetForm({ title: 'Змінити пароль', submit: 'Зберегти', ssid: n.ssid, fixed: true, mode: 'edit',
      save: async (v) => { await postNet(v); } });
    if (!ok) return;
    toast('Пароль оновлено.', 'ok');
    await act(async () => {});
  }
  function postNet(v) {
    const body = { ssid: v.ssid };
    if (v.password !== undefined) body.password = v.password;
    return api.post('/api/wifi/networks', body, T);
  }

  /* ---------- Дії зі списком ---------- */
  const move = (from, to) => act(() => api.post('/api/wifi/networks/move', { from, to }, T));

  async function del(n) {
    const text = 'Видалити мережу «' + n.ssid + '»?' + (n.current ? ' Це поточна мережа: пристрій лишиться підключеним, ' +
      'доки зʼєднання не перерветься, після цього вона вже не використовуватиметься.' : '');
    if (!(await confirmDialog(text, { okText: 'Видалити', danger: true }))) return;
    await act(async () => { await api.del('/api/wifi/networks/' + n.index, T); toast('Мережу видалено.', 'ok'); });
  }

  async function connect(n) {
    const text = 'Підключитись до «' + n.ssid + '» зараз? Зʼєднання з цією сторінкою обірветься, а IP-адреса пристрою може змінитись. ' +
      'Далі шукайте його за адресою ' + CONFIG.mdnsHost + ' або за IP, який показує екран пристрою. Радіо на кілька секунд перерветься.';
    if (busy || !(await confirmDialog(text, { okText: 'Підключитись' }))) return;
    busy = true; render();
    let r = null;
    try { r = await api.post('/api/wifi/connect', { index: n.index }, T); } catch (e) { toast(errText(e), 'error'); }
    if (!r || !r.switching) {
      if (r) toast('Це вже поточна мережа.');
      busy = false;
      await loadWifi();
      return;
    }
    poller.stop();                                        // далі лише очікування пристрою
    const title = 'Підключення до «' + n.ssid + '»…';
    const w = waitScreen(title, 'Пристрій перемикається на іншу мережу. Це може тривати до хвилини.', true);
    const ok = await waitForDevice({ delayMs: r.inMs || 0, timeoutMs: CONFIG.wifiSwitchTimeoutMs,
      onTick: (s) => w.set(title, 'Очікуємо відповіді: ' + s + ' с.', true) });
    if (ok) { location.reload(); return; }
    w.set('Пристрій не відповідає за цією адресою',
      'Він міг отримати іншу IP-адресу або ще підключається.\nПідключіть телефон чи комп’ютер до тієї самої мережі, що й контролер, ' +
      'і відкрийте http://' + CONFIG.mdnsHost + ' або адресу, яку показує екран пристрою.\nЯкщо мережа не підійшла, контролер сам повернеться до однієї зі збережених.',
      false, el('button', { class: 'btn btn-primary', type: 'button', onclick: () => location.reload() }, 'Оновити сторінку'));
  }

  /* ---------- Скан ---------- */
  async function scan() {
    if (busy || scanning) return;
    if (!scanWarned) {
      if (!(await confirmDialog('Під час сканування звук радіо може на кілька секунд просісти. Продовжити?', { okText: 'Сканувати' }))) return;
      scanWarned = true;
    }
    scanning = true; scanMsg = ''; render();
    try {
      try { await api.send('POST', '/api/wifi/scan', undefined, undefined, T); }
      catch (e) { if (!(e instanceof ApiError && e.code === 'wifi_scan_busy')) throw e; }   // скан уже триває - просто чекаємо
      const end = Date.now() + CONFIG.wifiScanTimeoutMs;
      for (;;) {
        await sleep(CONFIG.wifiScanPollMs);
        const r = await api.get('/api/wifi/scan');
        if (r.state === 'done') {
          results = Array.isArray(r.networks) ? r.networks : [];
          scanMsg = results.length ? 'Знайдено мереж: ' + results.length + '. Приховані мережі не показуються.' : 'Мереж не знайдено.';
          break;
        }
        if (r.state === 'failed') { scanMsg = 'Сканування не вдалося. Спробуйте ще раз.'; break; }
        if (Date.now() > end) { scanMsg = 'Сканування не завершилось вчасно. Спробуйте ще раз.'; break; }
      }
    } catch (e) { toast(errText(e), 'error'); }
    scanning = false;
    render();
  }

  /* ---------- Події ---------- */
  const rowAct = (ev) => {
    const b = ev.target.closest('button[data-act]');
    return b && !busy && !scanning ? [b.dataset.act, b.closest('li')] : null;
  };
  ui.nets.addEventListener('click', (ev) => {
    const a = rowAct(ev);
    const n = a && nets().find((x) => x.index === Number(a[1].dataset.i));
    if (!n) return;
    ({ up: () => move(n.index, n.index - 1), down: () => move(n.index, n.index + 1), connect: () => connect(n),
      edit: () => editNet(n), del: () => del(n) })[a[0]]();
  });
  ui.found.addEventListener('click', (ev) => {
    const a = rowAct(ev);
    const r = a && ui.found._list[Number(a[1].dataset.s)];
    if (!r) return;
    const saved = nets().find((x) => x.ssid === r.ssid);
    if (saved) editNet(saved); else addNet(r.ssid, true, r.secure ? 'required' : 'none');
  });
  ui.add.addEventListener('click', () => addNet('', false, 'optional'));
  ui.scanBtn.addEventListener('click', scan);

  initShell('wifi');
  onConnectionChange(render);
  render();
  poller.start();
})();
