'use strict';
/* Сторінка «Система» (Prompt 23; OTA — Prompt 24). Залежить від common.js і nav.js.
   /api/system важкий (обхід LittleFS): читаємо при відкритті та за кнопкою «Оновити», не опитуємо.
   OTA: сире тіло POST /api/ota через XMLHttpRequest (прогрес передачі), поза чергою api.*; на час передачі
   опитування зупинено, решта сторінки заблокована, закриття сторінки гальмує beforeunload. */
(() => {
  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), grp: g('grp'), info: g('info'), note: g('info-note'), refresh: g('btn-refresh'),
    reboot: g('btn-reboot'), factory: g('btn-factory'), wifiReset: g('btn-wifi'),
    cur: g('ota-cur'), otaGrp: g('ota-grp'), file: g('ota-file'), pick: g('ota-pick'), name: g('ota-name'),
    err: g('ota-err'), start: g('ota-start'), otaNote: g('ota-note'), prog: g('ota-prog'), bar: g('ota-bar'),
    stat: g('ota-stat'), cancel: g('ota-cancel'),
  };
  const RESET = {
    PowerOn: 'Увімкнення живлення', External: 'Зовнішній сигнал скидання', Software: 'Програмний перезапуск',
    Panic: 'Збій програми (panic)', IntWatchdog: 'Watchdog (переривання)', TaskWatchdog: 'Watchdog (задача)',
    OtherWatchdog: 'Watchdog', DeepSleep: 'Вихід із глибокого сну', Brownout: 'Просадка живлення',
    Sdio: 'SDIO', Other: 'Інша причина',
  };
  const WSTATE = {
    Connecting: 'Підключення', Connected: 'Підключено', ApMode: 'Точка доступу', ApClientConnected: 'Точка доступу (є клієнт)',
  };

  let st = null;              // /api/status
  let busy = false;
  let infoLoading = false;
  let sys = null;             // останній /api/system (дані «ДО» оновлення, ліміт образу)
  let picked = null;          // обраний і перевірений File
  let pickSeq = 0;
  let uploading = false;      // OTA з цієї сторінки триває (від старту до перезапуску / помилки)
  let sent = false;           // тіло передано повністю, чекаємо відповіді пристрою
  let xhr = null;
  let t0 = 0;

  const v = (x) => (x === undefined || x === null || x === '' ? '—' : String(x));
  const sizes = (a, b) => formatBytes(a) + ' з ' + formatBytes(b);

  function infoRows(s) {
    const f = s.firmware || {}, c = s.chip || {}, h = s.heap || {}, p = s.psram || {}, fs = s.fs || {};
    const o = s.ota || {}, w = s.wifi || {}, cfg = s.settings || {};
    return [
      ['Версія прошивки', v(f.version)],
      ['Збірка', [f.buildDate, f.buildTime].filter(Boolean).join(' ') || '—'],
      ['SDK', v(f.sdk)],
      ['Чип', v(c.model) + ', ревізія ' + v(c.revision) + ', ядер: ' + v(c.cores) + ', ' + v(c.cpuMhz) + ' МГц'],
      ['Flash', s.flash ? formatBytes(s.flash.sizeBytes) : '—'],
      ['Heap, вільно', sizes(h.free, h.total) + ' (мін. ' + formatBytes(h.min) + ', найбільший блок ' + formatBytes(h.largestBlock) + ')'],
      ['PSRAM, вільно', sizes(p.free, p.total)],
      ['LittleFS, зайнято', sizes(fs.usedBytes, fs.totalBytes)],
      ['OTA-розділ', 'зараз ' + v(o.runningPartition) + ', наступний ' + v(o.nextPartition) + ', образ до ' + formatBytes(o.maxImageBytes)],
      ['Причина скидання', RESET[s.resetReason] || v(s.resetReason)],
      ['Час роботи', formatUptime(s.uptimeMs)],
      ['Режим', v(s.mode)],
      ['Wi-Fi', [WSTATE[w.state] || w.state, w.mode, w.ssid && '«' + w.ssid + '»', w.ip, w.rssi && w.rssi + ' дБм']
        .filter(Boolean).join(' · ') || '—'],
      ['Записів налаштувань', v(cfg.writeCount) + (cfg.dirty ? ' (є незбережені зміни)' : '')],
    ];
  }

  async function loadInfo(manual) {
    if (infoLoading) return;
    infoLoading = true; render();
    try {
      const s = await api.get('/api/system', CONFIG.stationsTimeoutMs);
      sys = s; renderOtaCur();
      ui.info.replaceChildren(...infoRows(s).map(([l, val]) => el('tr', null, el('td', { class: 'muted' }, l), el('td', null, val))));
      setText(ui.note, 'Оновлено о ' + new Date().toLocaleTimeString('uk-UA'));
      if (manual) toast('Інформацію оновлено.', 'ok');
    } catch (e) {
      setText(ui.note, 'Не вдалося отримати дані: ' + errorText(e));
    }
    infoLoading = false; render();
  }

  /* Деструктивна дія: confirmDialog -> блокування кнопок -> запит. Повтор неможливий, поки busy. */
  async function act(fn) {
    if (busy) return;
    busy = true; render();
    try { await fn(); } catch (e) { toast(errorText(e), 'error'); }
    busy = false; render();
  }
  const stopAll = () => poller.stop();

  ui.reboot.addEventListener('click', async () => {
    if (busy || !(await confirmDialog('Перезапустити пристрій? Звук зупиниться приблизно на 10–15 секунд, налаштування не зміняться.',
      { okText: 'Перезапустити' }))) return;
    act(() => rebootAndWait('/api/system/reboot', undefined, stopAll));
  });

  ui.factory.addEventListener('click', async () => {
    if (busy || !(await confirmDialog('Скинути налаштування до заводських? Буде скинуто: тип аудіопроцесора, назви входів, яскравість і орієнтацію дисплея, ' +
      'звукові профілі всіх входів (гучність, бас, дискант, баланс, підсилення, тонкомпенсація), останній вхід і станцію. ' +
      'НЕ зміняться: список станцій, мапа пульта, збережена мережа Wi-Fi. Пристрій перезапуститься.',
      { okText: 'Скинути', danger: true }))) return;
    act(() => rebootAndWait('/api/system/factory-reset', { confirm: true }, stopAll));
  });

  ui.wifiReset.addEventListener('click', async () => {
    if (busy || !(await confirmDialog('Скинути Wi-Fi? Пристрій забуде збережену мережу й перезапуститься в режимі точки доступу «' + CONFIG.apSsid +
      '». Ця сторінка стане недоступною, доки ви не підключите пристрій до мережі через портал налаштування (' + CONFIG.apAddress +
      '). Налаштування, станції та мапа пульта не зміняться. Продовжити?', { okText: 'Скинути Wi-Fi', danger: true }))) return;
    act(async () => {
      await api.post('/api/wifi/reset', { confirm: true });
      stopAll();                                           // мережа зміниться: відповіді чекати немає сенсу
      waitScreen('Wi-Fi скинуто',
        'Пристрій перезапускається в режимі точки доступу. Ця сторінка більше не працюватиме.\n\n' +
        '1. Підключіть телефон чи ноутбук до Wi-Fi «' + CONFIG.apSsid + '».\n' +
        '2. Відкрийте ' + CONFIG.apAddress + ', якщо портал не відкрився сам.\n' +
        '3. Оберіть свою мережу й введіть пароль.\n\n' +
        'Після підключення пристрій знову буде за адресою ' + CONFIG.mdnsHost + '.', false);
    });
  });

  ui.refresh.addEventListener('click', () => loadInfo(true));

  /* ---------- OTA (P24) ---------- */
  const fw = (f) => { f = f || {}; return v(f.version) + ', збірка ' + ([f.buildDate, f.buildTime].filter(Boolean).join(' ') || '—'); };
  const maxImage = () => (sys && sys.ota && sys.ota.maxImageBytes) || CONFIG.otaMaxBytes;
  const guard = (e) => { e.preventDefault(); e.returnValue = ''; };

  function renderOtaCur() {
    if (!sys) { setText(ui.cur, 'Поточну версію не отримано.'); return; }
    const o = sys.ota || {};
    setText(ui.cur, 'Зараз: ' + fw(sys.firmware) + '. Образ — до ' + formatBytes(maxImage()) +
      '; запис піде в розділ ' + v(o.nextPartition) + ' (працює ' + v(o.runningPartition) + ').');
  }

  /* Чому зараз не можна почати OTA (за режимом пристрою) або null. */
  function blockReason(s) {
    if (!s) return null;
    if (s.mode === 'OtaUpdate') return 'Іде оновлення прошивки (' + (s.otaProgress || 0) + '%). Дочекайтесь завершення.';
    if (s.mode === 'IrLearn') return 'Іде навчання пульта. Спершу завершіть або скасуйте його.';
    if (s.mode === 'WifiSetup') return 'Пристрій налаштовує Wi-Fi.';
    return null;
  }

  /* Перевірки файлу до відправки -> текст помилки або null. */
  async function checkImage(f) {
    if (!/\.bin$/i.test(f.name)) return 'Потрібен файл із розширенням .bin (firmware.bin).';
    if (f.size === 0) return 'Файл порожній.';
    if (f.size < CONFIG.otaMinBytes) {
      return 'Файл замалий (' + formatBytes(f.size) + '): схоже, це не прошивка (можливо, bootloader.bin чи partitions.bin).';
    }
    if (f.size > maxImage()) return 'Файл завеликий: ' + formatBytes(f.size) + ' (максимум ' + formatBytes(maxImage()) + ').';
    let b;
    try { b = new Uint8Array(await f.slice(0, 1).arrayBuffer()); } catch (_) { return 'Не вдалося прочитати файл.'; }
    if (b[0] !== CONFIG.otaMagic) return 'Це не образ прошивки ESP32. Потрібен firmware.bin (littlefs.bin вантажити не потрібно).';
    return null;
  }

  function showPick(f, e) {
    setText(ui.name, f ? f.name + ' · ' + formatBytes(f.size) : 'Файл не обрано');
    setText(ui.err, e || ''); ui.err.hidden = !e;
  }

  ui.pick.addEventListener('click', () => ui.file.click());
  ui.file.addEventListener('change', async () => {
    const f = ui.file.files[0], seq = ++pickSeq;
    ui.file.value = '';
    if (!f) return;
    picked = null; setBanner('ota-result', null); render();
    const e = await checkImage(f);
    if (seq !== pickSeq) return;
    picked = e ? null : f;
    showPick(f, e); render();
  });

  function setProgress(loaded, total) {
    const pct = total ? Math.min(100, Math.floor(loaded * 100 / total)) : 0, sec = (Date.now() - t0) / 1000;
    const rate = sec >= 1 ? loaded / sec : 0;
    ui.bar.style.width = pct + '%';
    setAttr(ui.bar.parentNode, 'aria-valuenow', pct);
    setText(ui.stat, pct + '% · ' + formatBytes(loaded) + ' з ' + formatBytes(total) +
      (rate ? ' · ' + formatBytes(rate) + '/с · залишилось ~' + formatEta((total - loaded) / rate) : ''));
  }

  /* POST /api/ota: сире тіло, application/octet-stream (Content-Length ставить браузер). Поза чергою api.*. */
  function sendOta(f) {
    return new Promise((resolve, reject) => {
      const x = xhr = new XMLHttpRequest();
      x.open('POST', api.base() + '/api/ota');
      x.setRequestHeader('Content-Type', 'application/octet-stream');
      x.timeout = CONFIG.otaUploadTimeoutMs;
      x.upload.onprogress = (ev) => { if (ev.lengthComputable) setProgress(ev.loaded, ev.total); };
      x.upload.onload = () => {
        sent = true; setProgress(f.size, f.size);
        setText(ui.stat, 'Передано ' + formatBytes(f.size) + '. Пристрій перевіряє образ…'); render();
      };
      x.onload = () => {
        let d = null;
        try { d = JSON.parse(x.responseText); } catch (_) { /* не JSON */ }
        if (x.status >= 200 && x.status < 300 && d && d.ok !== false) resolve(d);
        else reject(new ApiError(x.status, d, d ? d.error : 'bad_response'));
      };
      x.onerror = () => reject(new ApiError(0, null, 'network'));
      x.ontimeout = () => reject(new ApiError(0, null, 'timeout'));
      x.onabort = () => reject(new ApiError(0, null, 'aborted'));
      x.send(f);
    });
  }

  function endUpload() {
    uploading = false; sent = false; xhr = null;
    window.removeEventListener('beforeunload', guard);
    poller.start();
    render();
  }

  function failUpload(e) {
    const c = e && e.code;
    let t = 'Оновлення не вдалося: ' + errorText(e);
    let kind = 'error';
    if (c === 'aborted') { t = 'Передачу скасовано. Запис не завершено, чинну прошивку не змінено.'; kind = 'info'; }
    else if (c === 'network' || c === 'timeout') {
      t = 'Звʼязок обірвано під час передачі. Стан пристрою невідомий: перевірте, чи він відповідає. ' +
        'Якщо запис не завершився, чинну прошивку не змінено.';
    }
    endUpload();
    setBanner('ota-result', t, kind);
  }

  function summary(b, a) {
    if (!a) return 'Пристрій відповідає, але дані про систему отримати не вдалося. Оновіть сторінку.';
    return ['Було: ' + (b ? fw(b.firmware) : '—'), 'Стало: ' + fw(a.firmware),
      'Час роботи: ' + formatUptime(a.uptimeMs) + ' · причина скидання: ' + (RESET[a.resetReason] || v(a.resetReason))].join('\n');
  }

  /* Після передачі: чекаємо перезапуск і показуємо підсумок «було / стало» (без висновків про успіх). */
  async function afterUpload(resp) {
    window.removeEventListener('beforeunload', guard);
    const before = sys;
    const reload = el('button', { class: 'btn btn-primary', type: 'button', onclick: () => location.reload() }, 'Оновити сторінку');
    const w = waitScreen('Прошивку передано', 'Пристрій перевіряє образ і перезапускається. Не вимикайте живлення й не закривайте сторінку.', true);
    if (resp && resp.restarting === false) {
      w.set('Потрібен ручний перезапуск', 'Прошивку записано, але пристрій не зміг перезапуститися сам. Вимкніть і ввімкніть живлення.', false, reload);
      return;
    }
    const ok = await waitForDevice({
      delayMs: CONFIG.otaRebootDelayMs, timeoutMs: CONFIG.otaRebootTimeoutMs,
      onTick: (s) => w.set('Пристрій перезапускається…', 'Очікуємо відповіді: ' + s + ' с. Не вимикайте живлення.', true),
    });
    if (!ok) {
      w.set('Пристрій не відповідає', 'Перевірте живлення й мережу. Якщо екран пристрою не оживає, можливо, потрібна перепрошивка по USB.', false, reload);
      return;
    }
    let after = null;
    try { after = await api.get('/api/system', CONFIG.stationsTimeoutMs); } catch (_) { /* покажемо без даних */ }
    w.set('Пристрій перезапущено', summary(before, after), false, reload);
  }

  async function startOta() {
    if (!picked || uploading || busy) return;
    const f = picked, e = await checkImage(f);
    if (e) { picked = null; showPick(f, e); render(); return; }
    const ok = await confirmDialog('Оновити прошивку з файлу «' + f.name + '» (' + formatBytes(f.size) + ')? ' +
      'Не вимикайте живлення й не закривайте сторінку до кінця. Звук зупиниться, а пристрій перезапуститься. ' +
      'Автоматичного відкату немає: якщо нова прошивка не запуститься, пристрій доведеться перепрошити по USB.',
      { okText: 'Оновити прошивку', danger: true });
    if (!ok) return;
    uploading = true; sent = false; t0 = Date.now();
    setBanner('ota-result', null); setProgress(0, f.size);
    window.addEventListener('beforeunload', guard);
    poller.stop();
    render();
    try {                                      // дочекатись черги api.* і перевірити режим свіжим статусом
      const s = await api.get('/api/status');
      const why = blockReason(s);
      if (why) throw new Error(why);
      st = s;
    } catch (e1) { endUpload(); setBanner('ota-result', 'Оновлення не розпочато: ' + errorText(e1), 'error'); return; }
    t0 = Date.now();
    let r;
    try { r = await sendOta(f); } catch (e2) {
      if (sent && e2.code === 'network') { afterUpload(null); return; }   // тіло дійшло, відповідь загубилась: ймовірно, перезапуск
      failUpload(e2); return;
    }
    afterUpload(r);
  }

  ui.start.addEventListener('click', startOta);
  ui.cancel.addEventListener('click', () => { if (xhr && !sent) xhr.abort(); });

  /* ---------- Рендер і опитування ---------- */
  function render() {
    const ota = !uploading && !!st && st.mode === 'OtaUpdate';
    setBanner('ota', ota ? 'Оновлення прошивки: ' + (st.otaProgress || 0) + '%. Сервісні дії та нове оновлення вимкнено.' : null,
      'warn', { progress: st && st.otaProgress });
    if (st) renderWifi(ui.wifi, st.wifi);
    ui.grp.disabled = !st || connection.offline || uploading;
    [ui.reboot, ui.factory, ui.wifiReset].forEach((b) => { b.disabled = busy || ota || uploading; });
    ui.refresh.disabled = infoLoading || connection.offline || uploading;
    const why = uploading ? null : blockReason(st);
    ui.otaGrp.disabled = !st || connection.offline || uploading || busy || !!why;
    ui.start.disabled = !picked;
    setText(ui.otaNote, why || ''); ui.otaNote.hidden = !why;
    ui.prog.hidden = !uploading; ui.cancel.hidden = sent;
  }
  async function refreshStatus() { st = await api.get('/api/status'); render(); }
  const poller = createPoller(refreshStatus, { intervalMs: CONFIG.settingsPollMs });

  initShell('system');
  onConnectionChange(render);
  render();
  loadInfo(false);
  poller.start();
})();
