'use strict';
/* Сторінка «Система» (Prompt 23). Залежить від common.js і nav.js.
   /api/system важкий (обхід LittleFS): читаємо при відкритті та за кнопкою «Оновити», не опитуємо.
   OTA (P24) додасться окремою карткою між «Інформацією» та «Сервісними діями». */
(() => {
  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), grp: g('grp'), info: g('info'), note: g('info-note'), refresh: g('btn-refresh'),
    reboot: g('btn-reboot'), factory: g('btn-factory'), wifiReset: g('btn-wifi'),
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

  /* ---------- Рендер і опитування ---------- */
  function render() {
    const ota = !!st && st.mode === 'OtaUpdate';
    setBanner('ota', ota ? 'Оновлення прошивки: ' + (st.otaProgress || 0) + '%. Сервісні дії вимкнено.' : null,
      'warn', { progress: st && st.otaProgress });
    if (st) renderWifi(ui.wifi, st.wifi);
    ui.grp.disabled = !st || connection.offline;
    [ui.reboot, ui.factory, ui.wifiReset].forEach((b) => { b.disabled = busy || ota; });
    ui.refresh.disabled = infoLoading || connection.offline;
  }
  async function refreshStatus() { st = await api.get('/api/status'); render(); }
  const poller = createPoller(refreshStatus, { intervalMs: CONFIG.settingsPollMs });

  initShell('system');
  onConnectionChange(render);
  render();
  loadInfo(false);
  poller.start();
})();
