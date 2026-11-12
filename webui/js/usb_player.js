'use strict';
/* Плеєр USB на «Головній» (USB16b-1; USB16b-2a: спільні хуки window.usbShared для браузера файлів; USB18b: картка треку з ID3, «N / M», seek за кінець). Підключати ПІСЛЯ common.js і nav.js, ПЕРЕД home.js.
   Контракт — docs/web_api.md §16 (/api/usb/*) і §17 (USB = вхід 3). Усе USB-специфічне (CONFIG, REASONS, іконки,
   запити) живе ТУТ, common.js не змінюється. Дані з флешки (імʼя файла, теги ID3) виводяться лише через textContent / el().
   Зовнішній інтерфейс: createUsbPlayer(mountEl) → {setActive(bool), setTitle(text)}. */
(() => {
  /* ---------- Розширення спільних об'єктів (лише нові ключі; наявні не перезаписуємо) ---------- */
  const addMissing = (target, src) => { Object.keys(src).forEach((k) => { if (!(k in target)) target[k] = src[k]; }); };

  addMissing(CONFIG, {
    usbStatusPollMs: 1000,   // GET /api/usb/status, лише поки активний вхід USB (контракт радить ≈ 2 Гц) — ПРИПУЩЕННЯ
    usbTickMs: 250,          // локальне оновлення часу / смуги між опитуваннями
    usbPauseHoldMs: 2500,    // після pause / resume ігноруємо paused зі status (прошивка змінює його за ≈ 1–2 с)
    usbOptHoldMs: 1500,      // після POST /api/usb/options ігноруємо shuffle / repeat зі status
    usbSeekHoldMs: 2500,     // після seek тримаємо позицію локально (нову позицію видно за ≈ 1–2 с)
    usbSeekStepSec: 5,       // крок смуги прогресу (точність перемотування ≈ 1–2 с)
    usbSeekMaxSec: 3600,     // межа deltaSec у контракті
    usbCmdPollMs: 300,       // GET /api/usb/result?id=N
    usbCmdTimeoutMs: 8000,   // скільки чекати результат команди з reqId — ПРИПУЩЕННЯ
    usbTagGraceMs: 800,      // USB18b: скільки тримати теги ТОГО САМОГО треку (той самий path), якщо знімок раптом без тегів — ПРИПУЩЕННЯ
  });

  addMissing(REASONS, {
    usb_mode_off: 'Режим USB не активний.',
    no_media: 'Флешку не знайдено.',
    stale: 'Список змінився. Оновіть його й спробуйте ще раз.',
    list_invalid: 'Список зараз недоступний. Спробуйте за мить.',
    not_dir: 'Це не папка.',
    not_file: 'Це не файл.',
    too_deep: 'Папка вкладена надто глибоко.',
    at_root: 'Це вже корінь флешки.',
    bad_index: 'Такого запису вже немає.',
    path_too_long: 'Шлях надто довгий.',
    open_failed: 'Не вдалося відкрити.',
    media_changed: 'Флешку змінено або вийнято.',
    queue_idle: 'Нічого не відтворюється.',
    end_of_queue: 'Це останній трек у черзі.',
    queue_overflow: 'У папці забагато файлів.',
    player_refused: 'Плеєр не зміг запустити трек.',
    not_playing: 'Зараз нічого не грає.',
    paused: 'На паузі перемотування недоступне. Спершу продовжіть відтворення.',
    seek_refused: 'Плеєр не зміг перемотати.',
    starting: 'Трек ще завантажується. Спробуйте за мить.',
    nothing_to_play: 'Немає що відтворювати. Оберіть файл.',
    not_active: 'Плеєр не активний.',
    queue_full: 'Пристрій ще виконує попередні команди. Спробуйте ще раз.',
    task_create_failed: 'Пристрою бракує ресурсів. Спробуйте ще раз.',
    usb_unavailable: 'USB зараз недоступний, потрібне перезавантаження пристрою.',
    unknown_req: 'Результат команди невідомий.',
    too_many_skips: 'Забагато пошкоджених файлів підряд.',
  });

  addMissing(ICONS, {
    shuffle: ['s', 'M4 7h3c4 0 6 10 10 10h3M17 14l3 3-3 3M4 17h3c1.4 0 2.6-.8 3.6-2M13.4 9c1-1.2 2.2-2 3.6-2h3M17 4l3 3-3 3'],
    repeat: ['s', 'M4 11V9a3 3 0 0 1 3-3h13M17 3l3 3-3 3M20 13v2a3 3 0 0 1-3 3H4M7 21l-3-3 3-3'],
  });

  /* Причини зупинки черги (queue.reason) → текст у блоці, коли нічого не грає. */
  const QUEUE_REASON = {
    end_of_queue: 'Кінець черги.',
    user_stop: 'Зупинено.',
    media_removed: 'Флешку вийнято.',
    mode_exit: 'Режим USB завершено.',
    too_many_skips: 'Забагато пошкоджених файлів підряд.',
  };

  /* ---------- Запити ---------- */
  const usbApi = {
    status: () => api.get('/api/usb/status'),
    result: (id) => api.get('/api/usb/result?id=' + encodeURIComponent(id)),
    control: (action) => api.post('/api/usb/control', { action }),
    seek: (deltaSec) => api.post('/api/usb/seek', { deltaSec }),
    options: (o) => api.post('/api/usb/options', o),
    // USB16b-2a: браузер файлів (usb_browser.js)
    list: (offset, limit, version) => api.get('/api/usb/list?offset=' + offset + '&limit=' + limit +
      (version === null || version === undefined ? '' : '&version=' + version)),
    cd: (body) => api.post('/api/usb/cd', body),
    play: (body) => api.post('/api/usb/play', body),
  };

  /* result.why — не ApiError, тому текст беремо з REASONS напряму. */
  const whyText = (why) => REASONS[why] || (why ? 'Помилка: ' + why : 'Команда не виконана.');
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  /* Чекає результат команди з reqId. alive() — чи ще потрібен результат (за замовчуванням завжди). */
  async function waitReqFor(id, alive) {
    const live = alive || (() => true);
    const until = Date.now() + CONFIG.usbCmdTimeoutMs;
    while (Date.now() < until && live()) {
      await sleep(CONFIG.usbCmdPollMs);
      try {
        const r = await usbApi.result(id);
        if (r.req && r.req.done) return r.req;
      } catch (e) {
        if (e instanceof ApiError && e.code === 'unknown_req') return { done: true, ok: null, why: 'unknown_req' };
        if (e instanceof ApiError && (e.code === 'network' || e.code === 'timeout')) continue;
        return null;
      }
    }
    return null;
  }

  /* Спільний доступ до останнього GET /api/usb/status (USB16b-2a): браузер файлів не опитує status сам. */
  const shared = { status: null, listeners: new Set() };
  const notifyStatus = () => { shared.listeners.forEach((fn) => { try { fn(shared.status); } catch (_) { /* слухач не ламає плеєр */ } }); };
  window.usbShared = {
    usbApi, whyText, waitReq: waitReqFor,
    getStatus: () => shared.status,
    onStatus(fn) { shared.listeners.add(fn); return () => shared.listeners.delete(fn); },
  };

  const tagStr = (v) => (typeof v === 'string' ? v.trim() : '');
  /* Імʼя файла без розширення (останню крапку не за першим символом вважаємо розширенням). */
  function fileBase(name) {
    const n = String(name || '');
    const k = n.lastIndexOf('.');
    return k > 0 ? n.slice(0, k) : n;
  }
  const trackKey = (np) => (np && (np.path || np.name)) || '';

  function fmtTime(sec) {
    const s = Math.max(0, Math.floor(sec));
    const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), r = s % 60;
    const p = (n) => String(n).padStart(2, '0');
    return h ? h + ':' + p(m) + ':' + p(r) : m + ':' + p(r);
  }

  /* ---------- Блок плеєра ---------- */
  function createUsbPlayer(mount) {
    const mkBtn = (cls, label, icon) => {
      const b = el('button', { type: 'button', class: cls, 'aria-label': label, title: label });
      setIcon(b, icon);
      return b;
    };
    const ui = {
      badge: el('span', { class: 'badge badge-muted' }, '…'),
      inputName: el('span', { class: 'muted' }),
      note: el('p', { class: 'hint usb-note', role: 'status', hidden: true }),
      title: el('div', { class: 'usb-title' }),
      sub: el('div', { class: 'usb-sub' }),
      count: el('span', { class: 'usb-count', 'aria-hidden': 'true' }),
      tCur: el('span', { class: 'usb-time' }, '0:00'),
      tDur: el('span', { class: 'usb-time' }, '—:—'),
      seek: el('input', { class: 'slider', type: 'range', min: 0, max: 1, step: CONFIG.usbSeekStepSec, value: 0,
                          'aria-label': 'Позиція у треку' }),
      prev: mkBtn('btn btn-icon btn-lg', 'Попередній трек', 'prev'),
      play: mkBtn('btn btn-primary btn-icon btn-xl', 'Грати', 'play'),
      next: mkBtn('btn btn-icon btn-lg', 'Наступний трек', 'next'),
      shuffle: el('button', { type: 'button', class: 'btn usb-opt', 'aria-pressed': 'false' }, 'Випадково'),
      repeat: el('button', { type: 'button', class: 'btn usb-opt', 'aria-pressed': 'false' }, 'Повтор'),
      stop: el('button', { type: 'button', class: 'btn usb-opt' }, 'Стоп'),
    };
    ui.meta = el('div', { class: 'usb-meta', hidden: true }, ui.sub, ui.count);
    ui.card = el('div', { class: 'usb-card', role: 'group' }, ui.title, ui.meta);
    setIcon(ui.shuffle, 'shuffle');
    setIcon(ui.repeat, 'repeat');
    setIcon(ui.stop, 'stop');

    mount.replaceChildren(
      el('legend', { class: 'visually-hidden' }, 'Плеєр USB'),
      el('div', { class: 'hero-head' }, ui.badge, ui.inputName),
      ui.note,
      ui.card,
      el('div', { class: 'usb-prog' }, ui.tCur, ui.seek, ui.tDur),
      el('div', { class: 'transport usb-transport' }, ui.prev, ui.play, ui.next),
      el('div', { class: 'usb-opts' }, ui.shuffle, ui.repeat, ui.stop));

    /* ---- Стан ---- */
    let s = null;                  // останній GET /api/usb/status
    let anchor = null;             // {pos, at, lockUntil}: база інтерполяції позиції (performance.now(), мс)
    let hold = null;               // {paused, until}: утримання paused після команди
    let optHold = null;            // {shuffle, repeat, until}
    let unavailable = false;       // /api/usb/status → 404 (збірка без режиму USB)
    let started = false;
    let dragging = false;
    let ctlBusy = false;           // іде запит control / options / seek
    let reqBusy = false;           // чекаємо результат команди з reqId (next / prev / stop)
    let endHold = null;            // {key, until}: seek за кінець (atEnd) — тримаємо кінець треку, доки не прийде НАСТУПНИЙ трек
    let tagHold = null;            // {key, tags, at}: останні теги треку з цим path (короткий «грейс» без мерехтіння)
    let tagHeld = false;           // зараз показано утримані теги (потрібне перемальовування за таймером)
    let ticker = null;

    const nowPlaying = () => (s && s.media === 'mounted' && s.nowPlaying) || null;   // без носія трек не показуємо
    const queue = () => (s && s.queue) || {};
    const statusPaused = () => {                   // пауза за останнім status (без утримання)
      const np = nowPlaying();
      return !!(np && np.paused) || !!queue().paused;
    };
    const pausedNow = () => (hold && Date.now() < hold.until ? hold.paused : statusPaused());
    const isRunning = () => !!nowPlaying() && queue().state === 'Playing' && !pausedNow();
    const optVal = (k) => (optHold && Date.now() < optHold.until ? optHold[k] : !!queue()[k]);

    function curPos() {
      const np = nowPlaying();
      if (!np || !anchor) return 0;
      let p = anchor.pos;
      if (isRunning()) p += (performance.now() - anchor.at) / 1000;
      const d = np.durSec || 0;
      if (d > 0) p = Math.min(p, d);
      return Math.max(0, p);
    }

    /* ---- Опитування ---- */
    async function refresh() {
      let r;
      try {
        r = await usbApi.status();
      } catch (e) {
        if (e instanceof ApiError && e.status === 404) { unavailable = true; shared.status = null; render(); notifyStatus(); }
        throw e;                                   // пулер збільшить інтервал; мережеві збої — банер «Немає звʼязку»
      }
      if (!started) return;                        // відповідь прийшла вже після зупинки
      unavailable = false;
      const prevPos = curPos(), prevKey = trackKey(nowPlaying()), wasRunning = isRunning();
      s = r;
      shared.status = r;
      if (hold && (hold.paused === statusPaused() || Date.now() >= hold.until)) hold = null;
      const np = nowPlaying();
      if (!np) { anchor = null; endHold = null; }
      else if (endHold && endHold.key === trackKey(np) && Date.now() < endHold.until) { /* seek за кінець: лишаємо кінець треку, доки status не покаже інший трек */ }
      else if (anchor && anchor.lockUntil > performance.now()) { /* після seek позицію зі status поки ігноруємо */ }
      else {
        endHold = null;
        const base = np.posSec || 0;
        const same = prevKey === trackKey(np) && wasRunning && anchor;
        // posSec має роздільність 1 с: щоб час не «підстрибував» назад між опитуваннями, лишаємо екстрапольоване значення
        anchor = { pos: same && Math.abs(prevPos - base) < 1.6 ? prevPos : base, at: performance.now(), lockUntil: 0 };
      }
      render();
      notifyStatus();
    }
    const poller = createPoller(refresh, { intervalMs: CONFIG.usbStatusPollMs });

    /* ---- Рендер ---- */
    function describe() {
      // → {badge:[текст, клас], note, ready}
      if (unavailable) return { badge: ['Недоступно', 'badge-err'], note: 'USB-плеєр недоступний у цій збірці прошивки.', ready: false };
      if (!s) return { badge: ['…', ''], note: 'Завантаження стану плеєра…', ready: false };
      const ms = s.mode && s.mode.state;
      if (ms === 'Entering') return { badge: ['Підключення…', 'badge-warn'], note: 'Підключення USB…', ready: false };
      if (ms === 'Exiting') return { badge: ['Вихід…', 'badge-warn'], note: 'Вихід із режиму USB…', ready: false };
      if (ms === 'Error') return { badge: ['Помилка USB', 'badge-err'], note: 'Помилка USB-входу. Перевірте флешку або оберіть інший вхід.', ready: false };
      if (!s.active) return { badge: ['USB вимкнено', ''], note: 'Режим USB не активний.', ready: false };
      if (s.media === 'none') return { badge: ['Немає флешки', 'badge-warn'], note: 'Флешку не знайдено. Вставте її — плеєр відновиться сам.', ready: false };
      if (s.media === 'mounting') return { badge: ['Підключення…', 'badge-warn'], note: 'Підключення флешки…', ready: false };
      if (s.media === 'fault') return { badge: ['Помилка носія', 'badge-err'], note: 'Помилка носія. Спробуйте вийняти й вставити флешку.', ready: false };
      const q = queue(), np = nowPlaying();
      if (q.state === 'Starting') return { badge: ['Завантаження…', 'badge-warn'], note: '', ready: true };
      if (np) return pausedNow() ? { badge: ['Пауза', 'badge-warn'], note: '', ready: true } : { badge: ['Грає', 'badge-ok'], note: '', ready: true };
      if (q.prepared) return { badge: ['Зупинено', ''], note: 'Натисніть ▶, щоб продовжити з місця зупинки.', ready: true };
      return { badge: ['Зупинено', ''], note: QUEUE_REASON[q.reason] || 'Нічого не грає.', ready: true };
    }

    function renderProgress() {
      const np = nowPlaying();
      const dur = np ? np.durSec || 0 : 0;
      const pos = curPos();
      const canSeek = !!np && dur > 0 && isRunning() && !ctlBusy;
      setAttr(ui.seek, 'max', dur > 0 ? dur : 1);                     // max — ПЕРЕД value
      if (!dragging) ui.seek.value = dur > 0 ? pos : 0;
      ui.seek.disabled = !canSeek;
      setAttr(ui.seek, 'title', np && !canSeek && dur > 0 && pausedNow() ? 'На паузі перемотування недоступне' : '');
      setText(ui.tCur, np ? fmtTime(dragging ? Number(ui.seek.value) : pos) : '0:00');
      setText(ui.tDur, np && dur > 0 ? fmtTime(dur) : '—:—');
    }

    /* Теги треку для показу. Теги ПОПЕРЕДНЬОГО треку не показуємо ніколи: утримання діє лише при тому самому path
       (знімок на 0-й секунді нового треку приходить без тегів — це норма, показуємо імʼя файла). */
    function tagsFor(np) {
      const own = { title: tagStr(np.title), artist: tagStr(np.artist), album: tagStr(np.album) };
      const key = typeof np.path === 'string' ? np.path : '';
      const now = Date.now();
      tagHeld = false;
      if (own.title || own.artist || own.album) { tagHold = key ? { key, tags: own, at: now } : null; return own; }
      if (key && tagHold && tagHold.key === key && now - tagHold.at < CONFIG.usbTagGraceMs) { tagHeld = true; return tagHold.tags; }
      if (!tagHold || tagHold.key !== key) tagHold = null;
      return own;
    }

    function renderCard() {
      const np = nowPlaying(), q = queue();
      if (!np) {
        tagHold = null; tagHeld = false;
        setText(ui.title, 'Нічого не грає'); setAttr(ui.title, 'title', '');
        setText(ui.sub, ''); setAttr(ui.sub, 'title', ''); setText(ui.count, ''); setAttr(ui.count, 'title', '');
        ui.meta.hidden = true;
        setAttr(ui.card, 'aria-label', 'Нічого не грає');
        return;
      }
      const tg = tagsFor(np);
      const title = tg.title || fileBase(np.name) || '—';
      const sub = tg.artist && tg.album ? tg.artist + ' — ' + tg.album : tg.artist || tg.album;
      const numbered = q.pos > 0 && q.count > 0 && q.pos <= q.count;     // queue.position НЕ використовуємо (у shuffle відстає)
      const cnt = numbered ? q.pos + ' / ' + q.count : '';
      setText(ui.title, title); setAttr(ui.title, 'title', title);
      setText(ui.sub, sub); setAttr(ui.sub, 'title', sub);
      setText(ui.count, cnt); setAttr(ui.count, 'title', numbered ? 'Трек ' + q.pos + ' з ' + q.count : '');
      ui.meta.hidden = !sub && !cnt;
      setAttr(ui.card, 'aria-label', 'Зараз грає: ' + title + (sub ? ', ' + sub : '') + (numbered ? ', трек ' + q.pos + ' з ' + q.count : ''));
    }

    function render() {
      const d = describe();
      const np = nowPlaying(), q = queue();
      setText(ui.badge, d.badge[0]);
      const bc = 'badge' + (d.badge[1] ? ' ' + d.badge[1] : '');
      if (ui.badge.className !== bc) ui.badge.className = bc;
      ui.note.hidden = !d.note;
      setText(ui.note, d.note);
      renderCard();

      const ready = d.ready, active = !!s && s.active === true && !unavailable;
      const idle = ctlBusy || reqBusy;
      const paused = pausedNow();
      const showPause = !!np && !paused;
      setIcon(ui.play, showPause ? 'pause' : 'play');
      setAttr(ui.play, 'aria-label', showPause ? 'Пауза' : 'Грати');
      setAttr(ui.play, 'title', showPause ? 'Пауза' : 'Грати');
      ui.play.disabled = !(ready && (np || q.prepared)) || ctlBusy || reqBusy;
      ui.prev.disabled = !ready || idle;
      ui.next.disabled = !active || idle;           // «Наступний» доступний, поки режим активний (відомий дефект: може завмирати без eof)
      ui.stop.disabled = !ready || idle;
      ui.shuffle.disabled = !active || ctlBusy;
      ui.repeat.disabled = !active || ctlBusy;
      setAttr(ui.shuffle, 'aria-pressed', active && optVal('shuffle'));
      setAttr(ui.repeat, 'aria-pressed', active && optVal('repeat'));
      renderProgress();
    }

    /* ---- Команди ---- */
    const waitReq = (id) => waitReqFor(id, () => started);

    async function doControl(action) {
      if (ctlBusy || reqBusy) return;
      ctlBusy = true; render();
      try {
        const r = await usbApi.control(action);
        if (typeof r.reqId === 'number') {            // next / prev / stop
          hold = null; anchor = null; endHold = null;
          reqBusy = true; render();
          const req = await waitReq(r.reqId);
          if (req && req.ok === false) toast(whyText(req.why), req.why === 'end_of_queue' ? 'info' : 'error');
        } else {                                       // pause / resume / toggle: пауза у status відстає на 1–2 с
          hold = { paused: r.result === 'paused', until: Date.now() + CONFIG.usbPauseHoldMs };
        }
      } catch (e) {
        hold = null;
        toast(errorText(e), 'error');
      }
      ctlBusy = false; reqBusy = false; render(); poller.now();
    }

    async function doOption(key) {
      if (ctlBusy) return;
      const want = !optVal(key);
      ctlBusy = true; render();
      try {
        const r = await usbApi.options({ [key]: want });
        optHold = { shuffle: !!r.shuffle, repeat: !!r.repeat, until: Date.now() + CONFIG.usbOptHoldMs };
      } catch (e) {
        toast(errorText(e), 'error');
      }
      ctlBusy = false; render(); poller.now();
    }

    async function doSeek(target) {
      const delta = clamp(Math.round(target - curPos()), -CONFIG.usbSeekMaxSec, CONFIG.usbSeekMaxSec);
      if (delta === 0) { render(); return; }
      ctlBusy = true; render();
      try {
        const r = await usbApi.seek(delta);
        const np = nowPlaying();
        if (r && r.atEnd === true && np) {
          // USB18b: seek за кінець = штатний кінець треку (не помилка). Утримання позиції зі seek НЕ ставимо: показуємо кінець
          // треку, доки status не покаже наступний трек (інший path), — стару позицію зі status ігноруємо лише до цього моменту.
          const dur = np.durSec || 0;
          endHold = { key: trackKey(np), until: Date.now() + CONFIG.usbSeekHoldMs };
          anchor = { pos: dur > 0 ? dur : target, at: performance.now(), lockUntil: 0 };
        } else {
          endHold = null;
          anchor = { pos: target, at: performance.now(), lockUntil: performance.now() + CONFIG.usbSeekHoldMs };
        }
      } catch (e) {
        toast(errorText(e), 'error');
        poller.now();
      }
      ctlBusy = false; render();
    }

    ui.play.addEventListener('click', () => doControl('toggle'));
    ui.prev.addEventListener('click', () => doControl('prev'));
    ui.next.addEventListener('click', () => doControl('next'));
    ui.stop.addEventListener('click', () => doControl('stop'));
    ui.shuffle.addEventListener('click', () => doOption('shuffle'));
    ui.repeat.addEventListener('click', () => doOption('repeat'));
    ui.seek.addEventListener('pointerdown', () => { dragging = true; });
    ['pointerup', 'pointercancel'].forEach((ev) => window.addEventListener(ev, () => { if (dragging) { dragging = false; renderProgress(); } }));
    ui.seek.addEventListener('input', () => { dragging = true; setText(ui.tCur, fmtTime(Number(ui.seek.value))); });
    ui.seek.addEventListener('change', () => { dragging = false; doSeek(Number(ui.seek.value)); });

    render();

    return {
      /* Вмикає / вимикає опитування: викликається з render() Головної (активний вхід USB і не standby). */
      setActive(on) {
        if (on === started) return;
        started = on;
        if (on) {
          poller.start();
          ticker = setInterval(() => { if (!document.hidden) { renderProgress(); if (tagHeld) renderCard(); } }, CONFIG.usbTickMs);
        } else {
          poller.stop();
          clearInterval(ticker); ticker = null;
          s = null; anchor = null; hold = null; optHold = null; unavailable = false; ctlBusy = false; reqBusy = false;
          endHold = null; tagHold = null; tagHeld = false;
          shared.status = null;
          render();
          notifyStatus();
        }
      },
      setTitle(text) { setText(ui.inputName, text || ''); },
    };
  }

  window.createUsbPlayer = createUsbPlayer;
})();

