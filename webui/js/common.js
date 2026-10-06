'use strict';
/* Спільний код усіх сторінок (Prompt 19). Підключати ПЕРШИМ. Опис: docs/web_ui.md. */

const CONFIG = {
  deviceName: 'Аудіоконтролер',
  apiTimeoutMs: 6000,        // тайм-аут одного запиту
  pollStatusMs: 1000,        // період опитування /api/status
  pollBackoffFactor: 2,      // множник інтервалу після кожної помилки
  pollBackoffMaxMs: 8000,    // стеля інтервалу при помилках
  offlineAfterFailures: 3,   // стільки збоїв звʼязку поспіль -> банер «Немає звʼязку»
  toastMs: 3500,
  toastErrorMs: 6000,
  toastMax: 3,
  rssiBars: [-55, -65, -75], // межі дБм для 4/3/2 рисок (слабше = 1 риска)
  devStorageKey: 'audioctl.device',
  deviceParam: 'device',
  stationsTimeoutMs: 10000,  // GET /api/stations (до 100 записів)
  // --- Головна ---
  volumeSendMinMs: 150,      // не частіше одного POST /api/volume за цей час під час перетягування
  volumeHoldMs: 1500,        // стільки після відпускання повзунка ігноруємо volume зі /api/status
  volumeButtonStep: 5,       // крок кнопок −/+ (одиниці шкали гучності)
  stationSearchThreshold: 12,// пошук показуємо, якщо станцій більше
  stationsRetryMs: 5000,     // пауза між невдалими автозавантаженнями списку
  marqueePxPerSec: 40,       // швидкість біжучого рядка
  marqueePauseS: 2,          // пауза на краях біжучого рядка
  resizeDebounceMs: 150,
  // --- Станції (Prompt 20) ---
  stationsPollMs: 3000,      // повільніше опитування /api/status на сторінці «Станції»
  stationsWriteTimeoutMs: 10000, // POST/PUT/DELETE/move (запис у flash)
  stationNameMaxBytes: 63,   // name: 1..63 байт UTF-8 (web_api.md §5)
  stationUrlMaxBytes: 191,   // url: ≤ 191 байт
  importMaxBytes: 131072,    // ліміт тіла POST /api/stations/import (≈ 128 КБ)
  importTimeoutMs: 60000,    // імпорт довгий: запис у flash
  searchDebounceMs: 150,
  // --- Аудіо (Prompt 21); createSlider використовує volumeSendMinMs / volumeHoldMs ---
  gainStepDb: { Tda7318: 6.25, Pt2313l: 3.75 },  // крок gain, дБ (web_api.md §2)
  audioNeutral: { tone: 0, balance: 0, loudness: false }, // нейтраль / дефолт (контракт не наводить; settings.h)
  // --- Пульт (Prompt 22) ---
  irLearnPollMs: 400,        // GET /api/ir/learn/status під час навчання (web_api.md: 300-500 мс)
  irStatusPollMs: 2000,      // /api/status на сторінці «Пульт»
  irImportMaxBytes: 8192,    // POST /api/ir/map/import (web_api.md §1)
  // --- Налаштування / Система (Prompt 23) ---
  settingsPollMs: 2000,      // /api/status на сторінках «Налаштування» і «Система»
  savedMs: 1800,             // скільки висить бейдж «Збережено»
  inputNameMaxBytes: 31,     // inputNames[i]: 1..31 байт UTF-8 (web_api.md §3.2)
  processorInputs: { Tda7318: 4, Pt2313l: 3 },   // скільки входів має чип (MASTER_SPEC §3)
  rebootPollMs: 1500,        // період опитування після reboot
  rebootPingMs: 2500,        // таймаут одного запиту під час очікування
  rebootTimeoutMs: 60000,    // скільки чекати, перш ніж здатися
  apSsid: 'AudioCtrl-Setup', // defaults::kApSsid
  apAddress: '192.168.4.1',  // адреса порталу в режимі AP (web_api.md §7)
  mdnsHost: 'audio.local',   // defaults::kMdnsName + .local
  // --- OTA (Prompt 24) ---
  otaMinBytes: 262144,       // менше 256 КБ - не прошивка (bootloader.bin, partitions.bin); сервер приймає від 4096 Б
  otaMaxBytes: 5767168,      // запасний ліміт, якщо /api/system недоступний (ota.maxImageBytes = слот app0/app1)
  otaMagic: 0xE9,            // перший байт образу ESP32
  otaUploadTimeoutMs: 300000,// XHR POST /api/ota: передача + перевірка образу
  otaRebootDelayMs: 1500,    // пауза перед опитуванням: пристрій перезапускається ~через 1.5 с після відповіді
  otaRebootTimeoutMs: 90000, // скільки чекати повернення пристрою після OTA (довше, ніж після reboot)
  // --- Рівень станції (Prompt 25b; web_api.md §5) ---
  stationLevelMinDb: -24,    // levelDb: ціле, лише послаблення
  stationLevelMaxDb: 0,
  stationLevelDefaultDb: -20, // нова станція, M3U / PLS, старий список (P25c: було -6; збігається з kDefaultStationLevelDb)
};

/* ---------- Помилки API ---------- */
class ApiError extends Error {
  constructor(status, data, code) {
    super(code || 'error');
    this.status = status;           // HTTP-код (0 = немає відповіді)
    this.data = data || {};         // розібране JSON-тіло
    this.code = code || (data && data.error) || 'error';
    this.reason = (data && data.reason) || null;
  }
}

/* Словник кодів -> українські повідомлення. Ключ: reason, потім error. */
const REASONS = {
  network: 'Немає звʼязку з пристроєм.',
  timeout: 'Пристрій не відповів вчасно.',
  bad_response: 'Пристрій надіслав незрозумілу відповідь.',
  // Відмови керування (409)
  standby: 'Пристрій у режимі очікування.',
  ota_in_progress: 'Іде оновлення прошивки.',
  ir_learn_active: 'Іде навчання пульта. Спершу завершіть або скасуйте його.',
  not_radio_input: 'Це працює лише на вході «WiFi Radio».',
  wifi_setup: 'Пристрій налаштовує Wi-Fi.',
  no_stations: 'Список станцій порожній.',
  input_unavailable: 'Цей вхід недоступний для поточного аудіопроцесора.',
  station_out_of_range: 'Такої станції немає в списку.',
  index_out_of_range: 'Такої станції немає в списку.',
  volume_out_of_range: 'Гучність поза допустимими межами.',
  gain_out_of_range: 'Підсилення поза допустимими межами.',
  out_of_range: 'Значення поза допустимими межами.',
  i2c_failed: 'Аудіопроцесор не відповів.',
  not_supported: 'Аудіопроцесор цього не підтримує.',
  audio_unavailable: 'Аудіопроцесор недоступний.',
  busy: 'Пристрій зайнятий. Спробуйте ще раз.',
  restart_pending: 'Пристрій уже перезапускається.',
  // Загальні помилки запиту
  invalid_value: 'Недопустиме значення.',
  invalid_json: 'Некоректний запит.',
  unknown_field: 'Невідоме поле в запиті.',
  conflicting_fields: 'Суперечливі поля в запиті.',
  missing_field: 'Бракує обовʼязкового поля.',
  no_body: 'Порожній запит.',
  body_too_large: 'Запит завеликий.',
  confirm_required: 'Потрібне підтвердження дії.',
  not_found: 'Не знайдено.',
  storage_error: 'Помилка запису в памʼять пристрою.',
  list_full: 'Список станцій заповнений.',
  // Станції, імпорт
  import_failed: 'Не вдалося імпортувати файл.',
  import_busy: 'Інший імпорт ще триває.',
  unknown_format: 'Невідомий формат файлу.',
  parse_error: 'Файл має помилки і не розпізнаний.',
  too_many_stations: 'У файлі забагато станцій.',
  empty: 'Файл порожній.',
  version_mismatch: 'Непідтримувана версія формату файлу.',
  too_big: 'Файл завеликий.',
  file_not_found: 'Файл не знайдено на пристрої.',
  out_of_memory: 'Пристрою бракує памʼяті.',
  io_error: 'Помилка читання або запису.',
  storage_write_failed: 'Не вдалося записати список у памʼять пристрою.',
  upload_failed: 'Помилка приймання файлу.',
  invalid_index: 'Некоректний номер станції.',
  name_required: 'Введіть назву.',
  name_too_long: 'Назва задовга.',
  name_invalid: 'Назва містить недопустимі символи.',
  url_required: 'Введіть адресу потоку.',
  url_too_long: 'Адреса задовга.',
  url_invalid: 'Некоректна адреса.',
  url_invalid_scheme: 'Адреса має починатися з http:// або https://',
  level_invalid: 'Рівень має бути цілим числом.',
  level_out_of_range: 'Рівень поза допустимими межами.',
  // OTA
  bad_image: 'Це не образ прошивки.',
  bad_magic: 'Це не образ прошивки.',
  too_small: 'Файл замалий для прошивки.',
  image_too_large: 'Прошивка не вміщається у розділ.',
  verify_failed: 'Прошивка не пройшла перевірку.',
  ota_busy: 'Оновлення вже триває.',
  length_required: 'Не вказано розмір файлу.',
  unsupported_content_type: 'Непідтримуваний тип вмісту.',
  incomplete: 'Пристрій отримав не весь файл: передачу обірвано.',
  begin_failed: 'Пристрій не зміг почати запис прошивки.',
  write_failed: 'Помилка запису прошивки в памʼять пристрою.',
  no_ota_partition: 'На пристрої немає вільного розділу для прошивки.',
  ota_done_restarting: 'Прошивку вже записано, пристрій перезапускається.',
  // IR
  learning_active: 'Іде навчання пульта.',
  already_learning: 'Навчання вже розпочато.',
  cannot_start: 'Зараз не можна почати навчання.',
  not_learnable: 'Цю дію не можна навчити.',
  unknown_action: 'Невідома дія.',
  invalid_action: 'Некоректна назва дії.',
  no_code_for_action: 'Для цієї кнопки немає навченого коду.',
  not_in_conflict: 'Конфлікту вже немає.',
  invalid_name: 'Недопустима назва входу.',
  expected_array_of_4: 'Некоректний список назв входів.',
  empty_request: 'Порожній запит.',
};

function errorText(e) {
  if (!(e instanceof ApiError)) return (e && e.message) || 'Невідома помилка.';
  if (e.status === 502 && e.code === 'error') return REASONS.i2c_failed;   // POST /api/settings: збій I2C
  let t = REASONS[e.reason] || REASONS[e.code];
  if (!t) return 'Помилка: ' + (e.reason || e.code || e.status);
  const d = (Array.isArray(e.data.errors) && e.data.errors[0]) || e.data;   // POST /api/settings кладе min/max у errors[0]
  if ((e.code === 'out_of_range' || e.code === 'level_out_of_range') && d.min !== undefined && d.max !== undefined) {
    t = t.replace(/\.$/, '') + ' (' + d.min + '…' + d.max + ').';
  }
  return t;
}

/* ---------- Утиліти DOM ---------- */
const $ = (sel, root) => (root || document).querySelector(sel);

/* el('div', {class:'x', onclick: fn, 'aria-label':'..'}, 'текст', childNode)
   Рядки завжди вставляються як текст (textContent), НІКОЛИ як HTML. */
function el(tag, attrs, ...children) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') n.className = v;
    else if (k.startsWith('on') && typeof v === 'function') n.addEventListener(k.slice(2), v);
    else n.setAttribute(k, v === true ? '' : String(v));
  }
  const add = (c) => {
    if (c === null || c === undefined || c === false) return;
    if (Array.isArray(c)) c.forEach(add);
    else n.append(c instanceof Node ? c : document.createTextNode(String(c)));
  };
  children.forEach(add);
  return n;
}

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

/* Записує текст/атрибут лише якщо змінився (менше перемальовувань). */
function setText(node, text) { if (node.textContent !== text) node.textContent = text; }
function setAttr(node, name, value) {
  if (node.getAttribute(name) !== String(value)) node.setAttribute(name, String(value));
}

/* ---------- Помічники (Prompt 20) ---------- */
function debounce(fn, ms) {
  let t = null;
  return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
}
/* Довжина рядка в байтах UTF-8 (сервер рахує байти, не символи). */
function byteLength(s) { return new TextEncoder().encode(String(s)).length; }
/* +3 / 0 / −3 (справжній мінус). */
function fmtSigned(v) { return v > 0 ? '+' + v : v < 0 ? '\u2212' + (-v) : '0'; }
function formatBytes(n) {
  if (typeof n !== 'number' || isNaN(n)) return '—';
  return n < 1024 ? n + ' Б' : n < 1048576 ? (n / 1024).toFixed(1) + ' КБ' : (n / 1048576).toFixed(1) + ' МБ';
}
/* Час роботи: «3 діб 4 год 5 хв», до хвилини - «42 с». */
function formatUptime(ms) {
  const s = Math.floor(ms / 1000), d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  if (typeof ms !== 'number' || isNaN(ms)) return '—';
  if (!d && !h && !m) return s + ' с';
  return [d && d + ' діб', h && h + ' год', m + ' хв'].filter(Boolean).join(' ');
}
/* Залишок часу: «8 с», «1 хв 5 с» (Prompt 24). */
function formatEta(sec) {
  if (typeof sec !== 'number' || !isFinite(sec) || sec < 0) return '—';
  const s = Math.ceil(sec), m = Math.floor(s / 60);
  return m ? m + ' хв ' + (s % 60) + ' с' : s + ' с';
}
/* 'YYYY-MM-DD' за локальним часом - для імен файлів. */
function fileStamp() {
  const d = new Date(); const p = (n) => String(n).padStart(2, '0');
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate());
}
/* Зберегти Blob як файл (працює і в dev-режимі, бо Blob уже отриманий через api.blob). */
function downloadBlob(blob, name) {
  const url = URL.createObjectURL(blob);
  const a = el('a', { href: url, download: name });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

/* ---------- Іконки (інлайн-SVG) ---------- */
const ICONS = {
  power: ['s', 'M12 3v8M7.1 6.7a7 7 0 1 0 9.8 0'],
  play: ['f', 'M8 5v14l11-7z'],
  pause: ['f', 'M6 5h4v14H6zM14 5h4v14h-4z'],
  stop: ['f', 'M6 6h12v12H6z'],
  prev: ['f', 'M6 6h2v12H6zM20 6v12L9 12z'],
  next: ['f', 'M16 6h2v12h-2zM4 6v12l11-6z'],
  volume: ['s', 'M4 9v6h4l5 4V5L8 9zM16.5 8.5a5 5 0 0 1 0 7'],
  mute: ['s', 'M4 9v6h4l5 4V5L8 9zM17 9.5l4 5M21 9.5l-4 5'],
  minus: ['s', 'M5 12h14'],
  plus: ['s', 'M12 5v14M5 12h14'],
  refresh: ['s', 'M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7'],
  search: ['s', 'M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14zM20 20l-4-4'],
  edit: ['s', 'M4 20h4L19 9l-4-4L4 16zM13.5 6.5l4 4'],
  trash: ['s', 'M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3'],
  'arrow-up': ['s', 'M12 19V5M6 11l6-6 6 6'],
  'arrow-down': ['s', 'M12 5v14M6 13l6 6 6-6'],
  move: ['s', 'M8 20V4M4 8l4-4 4 4M16 4v16M12 16l4 4 4-4'],
  upload: ['s', 'M12 16V4M7 9l5-5 5 5M4 20h16'],
  download: ['s', 'M12 4v12M7 11l5 5 5-5M4 20h16'],
  check: ['s', 'M5 12.5l4.5 4.5L19 7'],
  close: ['s', 'M6 6l12 12M18 6L6 18'],
  reset: ['s', 'M4 11a8 8 0 1 1 2.3 5.7M4 4v7h7'],
};
const SVG_NS = 'http://www.w3.org/2000/svg';

function icon(name) {
  const [kind, d] = ICONS[name] || ICONS.stop;
  const svg = document.createElementNS(SVG_NS, 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('aria-hidden', 'true');
  const p = document.createElementNS(SVG_NS, 'path');
  p.setAttribute('d', d);
  if (kind === 'f') p.setAttribute('fill', 'currentColor');
  else {
    p.setAttribute('fill', 'none'); p.setAttribute('stroke', 'currentColor');
    p.setAttribute('stroke-width', '2'); p.setAttribute('stroke-linecap', 'round');
    p.setAttribute('stroke-linejoin', 'round');
  }
  svg.append(p);
  return svg;
}

function setIcon(node, name) {
  if (node.dataset.iconName === name) return;
  node.dataset.iconName = name;
  node.querySelectorAll(':scope > svg').forEach((s) => s.remove());
  node.prepend(icon(name));
}

/* Підставляє іконки в усі елементи з data-icon="імʼя". */
function hydrateIcons(root) {
  (root || document).querySelectorAll('[data-icon]').forEach((n) => setIcon(n, n.dataset.icon));
}

/* ---------- API-клієнт ---------- */
const api = (() => {
  let base = '';
  try {
    const p = new URLSearchParams(location.search).get(CONFIG.deviceParam);
    if (p !== null) {
      if (p === '') sessionStorage.removeItem(CONFIG.devStorageKey);
      else if (/^[A-Za-z0-9.\-]+(:\d{1,5})?$/.test(p)) sessionStorage.setItem(CONFIG.devStorageKey, p);
    }
    const d = sessionStorage.getItem(CONFIG.devStorageKey);
    if (d) base = 'http://' + d;
  } catch (_) { /* sessionStorage недоступний - працюємо без dev-режиму */ }

  // ОДНА послідовна черга для всіх запитів: ESP має мало одночасних зʼєднань.
  let chain = Promise.resolve();
  function enqueue(job) {
    const p = chain.then(job, job);
    chain = p.catch(() => {});
    return p;
  }

  async function run(method, path, body, timeoutMs, opt) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), timeoutMs || CONFIG.apiTimeoutMs);
    const init = { method, cache: 'no-store', signal: ctl.signal, headers: {} };
    if (body !== undefined) {
      if (opt && opt.type) { init.headers['Content-Type'] = opt.type; init.body = body; }   // сире тіло (File/Blob/рядок)
      else { init.headers['Content-Type'] = 'application/json'; init.body = JSON.stringify(body); }
    }
    let res;
    try {
      res = await fetch(base + path, init);
    } catch (e) {
      throw new ApiError(0, null, e && e.name === 'AbortError' ? 'timeout' : 'network');
    } finally {
      clearTimeout(timer);
    }
    if (opt && opt.blob && res.ok) {                 // завантаження файлу (експорт)
      try { return await res.blob(); } catch (_) { throw new ApiError(res.status, null, 'network'); }
    }
    let data = null;
    try { data = await res.json(); } catch (_) { /* не JSON */ }
    if (!res.ok || (data && !Array.isArray(data) && data.ok === false)) {
      if (data && Array.isArray(data.errors) && data.errors[0]) {   // POST /api/settings
        throw new ApiError(res.status, data, data.errors[0].code);
      }
      throw new ApiError(res.status, data, data ? data.error : 'bad_response');
    }
    if (data === null) throw new ApiError(res.status, null, 'bad_response');
    return data;
  }

  return {
    base: () => base,
    get: (path, timeoutMs) => enqueue(() => run('GET', path, undefined, timeoutMs)),
    post: (path, body, timeoutMs) => enqueue(() => run('POST', path, body === undefined ? {} : body, timeoutMs)),
    put: (path, body, timeoutMs) => enqueue(() => run('PUT', path, body === undefined ? {} : body, timeoutMs)),
    del: (path, timeoutMs) => enqueue(() => run('DELETE', path, undefined, timeoutMs)),
    /* Сире тіло: send('POST', path, fileOrBlob, 'audio/x-mpegurl', timeoutMs). Відповідь - JSON. */
    send: (method, path, body, type, timeoutMs) => enqueue(() => run(method, path, body, timeoutMs, { type })),
    /* GET файлу -> Promise<Blob>. */
    blob: (path, timeoutMs) => enqueue(() => run('GET', path, undefined, timeoutMs, { blob: true })),
  };
})();

/* ---------- Toast ---------- */
function toast(text, kind) {
  let box = $('#toasts');
  if (!box) {
    box = el('div', { id: 'toasts', class: 'toasts', role: 'status', 'aria-live': 'polite' });
    document.body.append(box);
  }
  const k = kind === 'error' || kind === 'ok' ? kind : 'info';
  const n = el('div', { class: 'toast toast-' + k }, text);
  box.append(n);
  while (box.children.length > CONFIG.toastMax) box.firstChild.remove();
  setTimeout(() => n.remove(), k === 'error' ? CONFIG.toastErrorMs : CONFIG.toastMs);
}

/* ---------- Діалог підтвердження ---------- */
function confirmDialog(text, opts) {
  const o = Object.assign({ okText: 'Так', cancelText: 'Скасувати', danger: false }, opts);
  if (typeof HTMLDialogElement === 'undefined') return Promise.resolve(window.confirm(text));
  return new Promise((resolve) => {
    const dlg = el('dialog', { class: 'dialog' },
      el('form', { method: 'dialog' },
        el('p', null, text),
        el('div', { class: 'dialog-actions' },
          el('button', { class: 'btn', value: 'cancel', autofocus: true }, o.cancelText),
          el('button', { class: 'btn ' + (o.danger ? 'btn-danger' : 'btn-primary'), value: 'ok' }, o.okText))));
    dlg.addEventListener('close', () => { const ok = dlg.returnValue === 'ok'; dlg.remove(); resolve(ok); });
    document.body.append(dlg);
    dlg.showModal();
  });
}

/* ---------- Банери ---------- */
/* setBanner('id', 'текст', 'info'|'warn'|'error', {progress: 0..100}); text=null -> прибрати. */
function setBanner(id, text, kind, opts) {
  const box = $('#banners');
  if (!box) return;
  let n = box.querySelector('[data-banner="' + id + '"]');
  if (text === null || text === undefined) { if (n) n.remove(); return; }
  if (!n) {
    n = el('div', { class: 'banner', role: 'status', 'data-banner': id },
      el('span'), el('div', { class: 'progress', hidden: true }, el('i')));
    box.append(n);
  }
  n.className = 'banner' + (kind === 'warn' ? ' banner-warn' : kind === 'error' ? ' banner-error' : '');
  setText(n.firstChild, text);
  const bar = n.lastChild;
  const pr = opts && typeof opts.progress === 'number' ? clamp(opts.progress, 0, 100) : null;
  bar.hidden = pr === null;
  if (pr !== null) bar.firstChild.style.width = pr + '%';
}

/* ---------- Повзунок з тротлінгом (Prompt 21) ---------- */
/* createSlider(input, {send(v) async, onInput(v), onFail()}) -> {held, hold, release, send}.
   send кидає ApiError при відмові: toast + onFail (відкат). held() = тримають або ще
   CONFIG.volumeHoldMs після відпускання: поки так, опитування не чіпає повзунок. */
function createSlider(input, o) {
  let dragging = false, holdUntil = 0, pending = null, inflight = false, lastSent = 0, timer = null;
  const hold = () => { holdUntil = Date.now() + CONFIG.volumeHoldMs; };
  async function flush() {
    if (inflight || pending === null) return;
    const v = pending;
    pending = null; inflight = true; lastSent = Date.now();
    try { await o.send(v); } catch (e) {
      toast(errorText(e), 'error'); holdUntil = 0; pending = null;
      if (o.onFail) o.onFail();
    }
    inflight = false;
    if (pending !== null) queue(pending, false);
  }
  function queue(v, final) {
    pending = v;
    if (final) { clearTimeout(timer); timer = null; flush(); return; }
    if (timer || inflight) return;
    timer = setTimeout(() => { timer = null; flush(); }, Math.max(0, CONFIG.volumeSendMinMs - (Date.now() - lastSent)));
  }
  input.addEventListener('pointerdown', () => { dragging = true; });
  ['pointerup', 'pointercancel'].forEach((ev) => window.addEventListener(ev, () => {
    if (dragging) { dragging = false; hold(); }
  }));
  input.addEventListener('input', () => { const v = Number(input.value); hold(); if (o.onInput) o.onInput(v); queue(v, false); });
  input.addEventListener('change', () => { hold(); queue(Number(input.value), true); });
  return {
    held: () => dragging || Date.now() < holdUntil,
    hold, release: () => { holdUntil = 0; },
    send: (v) => { hold(); queue(v, true); },     // для кнопок −/+/«0»
  };
}

/* ---------- Звʼязок з пристроєм ---------- */
const connection = { offline: false, listeners: [] };
function setOffline(flag) {
  if (connection.offline === flag) return;
  connection.offline = flag;
  setBanner('offline', flag ? 'Немає звʼязку з пристроєм. Повторюємо спроби…' : null, 'error');
  connection.listeners.forEach((f) => f(flag));
}
function onConnectionChange(fn) { connection.listeners.push(fn); }

/* ---------- Опитування ---------- */
/* createPoller(fn, {intervalMs}) -> {start, stop, now}.
   fn: async () => {...}, кине помилку = невдача. Запити не накладаються: наступний -
   лише після завершення попереднього. Пауза при document.hidden; інтервал росте
   при помилках; збої ЗВʼЯЗКУ (network/timeout) вмикають банер «Немає звʼязку». */
function createPoller(fn, opts) {
  const base = (opts && opts.intervalMs) || CONFIG.pollStatusMs;
  let timer = null; let running = false; let again = false; let fails = 0; let active = false;

  function schedule(ms) { clearTimeout(timer); timer = setTimeout(tick, ms); }

  async function tick() {
    if (!active || document.hidden) return;       // відновиться з visibilitychange
    if (running) { again = true; return; }
    clearTimeout(timer);
    running = true;
    try {
      await fn();
      fails = 0;
      setOffline(false);
    } catch (e) {
      fails++;
      if (e instanceof ApiError && (e.code === 'network' || e.code === 'timeout') &&
          fails >= CONFIG.offlineAfterFailures) setOffline(true);
    }
    running = false;
    if (!active) return;
    if (again) { again = false; schedule(0); return; }
    schedule(fails ? Math.min(base * Math.pow(CONFIG.pollBackoffFactor, fails), CONFIG.pollBackoffMaxMs) : base);
  }

  document.addEventListener('visibilitychange', () => { if (!document.hidden) tick(); });
  return {
    start() { active = true; tick(); },
    stop() { active = false; clearTimeout(timer); },
    now() { tick(); },
  };
}

/* ---------- Wi-Fi ---------- */
function rssiToBars(rssi) {
  if (!rssi) return 0;
  const [a, b, c] = CONFIG.rssiBars;
  return rssi >= a ? 4 : rssi >= b ? 3 : rssi >= c ? 2 : 1;
}
/* Малює індикатор у <span class="wifi">; wifi = об'єкт status.wifi. */
function renderWifi(node, wifi) {
  if (!node.firstChild) for (let i = 0; i < 4; i++) node.append(el('i'));
  const ok = !!(wifi && wifi.connected);
  const bars = ok ? rssiToBars(wifi.rssi) : 0;
  [...node.children].forEach((b, i) => b.classList.toggle('on', i < bars));
  const label = ok ? 'Wi-Fi: ' + (wifi.ssid || '') + ', ' + wifi.rssi + ' дБм' : 'Wi-Fi не підключено';
  setAttr(node, 'aria-label', label);
  setAttr(node, 'title', label);
}

/* ---------- Перезапуск пристрою (Prompt 23) ---------- */
/* Повноекранне очікування. set(заголовок, текст, спінер, кнопка?) змінює вміст. Сторінка під ним стає inert. */
function waitScreen(title, text, spin) {
  const t = el('h2'), p = el('p', { class: 'muted' }), b = el('div', { class: 'wait-act' });
  const sp = el('i', { class: 'spinner', 'aria-hidden': 'true' });
  document.body.append(el('div', { class: 'wait', role: 'status' }, el('div', { class: 'wait-box' }, sp, t, p, b)));
  document.querySelectorAll('body > header, body > main').forEach((n) => { n.inert = true; });
  const set = (ti, te, s, btn) => { setText(t, ti); setText(p, te); sp.hidden = !s; b.replaceChildren(...(btn ? [btn] : [])); };
  set(title, text, spin);
  return { set };
}

/* Чекає, поки пристрій знову відповість після перезапуску. true - відповів, false - таймаут.
   «Відповів» = була недоступність АБО uptimeMs менший за час очікування (швидкий reboot не встигли побачити). */
async function waitForDevice(o) {
  o = Object.assign({ delayMs: 0, timeoutMs: CONFIG.rebootTimeoutMs, intervalMs: CONFIG.rebootPollMs, onTick: null }, o);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const t0 = Date.now();
  let down = false;
  await sleep(o.delayMs);
  while (Date.now() - t0 < o.timeoutMs) {
    try {
      const s = await api.get('/api/status', CONFIG.rebootPingMs);
      if (down || s.uptimeMs < Date.now() - t0) return true;
    } catch (_) { down = true; }
    if (o.onTick) o.onTick(Math.round((Date.now() - t0) / 1000));
    await sleep(o.intervalMs);
  }
  return false;
}

/* POST reboot / factory-reset -> екран очікування -> перезавантаження сторінки.
   Помилка самого запиту кидається (викликач показує toast). before() - зупинити опитування сторінки. */
async function rebootAndWait(path, body, before) {
  const r = body === undefined ? await api.send('POST', path) : await api.post(path, body);
  if (before) before();
  const w = waitScreen('Пристрій перезапускається…', 'Зазвичай це займає 10–15 секунд. Не вимикайте живлення.', true);
  const ok = await waitForDevice({
    delayMs: (r && r.inMs) || 0,
    onTick: (s) => w.set('Пристрій перезапускається…', 'Очікуємо відповіді: ' + s + ' с. Не вимикайте живлення.', true),
  });
  if (ok) { location.reload(); return; }
  w.set('Пристрій не відповідає', 'Перевірте живлення та мережу, потім оновіть сторінку.', false,
    el('button', { class: 'btn btn-primary', type: 'button', onclick: () => location.reload() }, 'Оновити сторінку'));
}

/* ---------- Каркас сторінки ---------- */
/* initShell('home'): заголовок, іконки, навігація (nav.js), dev-банер. */
function initShell(activeId) {
  const brand = $('#brand');
  if (brand) brand.textContent = CONFIG.deviceName;
  hydrateIcons();
  if (typeof renderNav === 'function') renderNav(activeId);
  if (api.base()) setBanner('dev', 'Режим розробки: пристрій ' + api.base().slice(7), 'info');
}
