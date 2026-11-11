'use strict';
/* Браузер файлів USB на «Головній» (USB16b-2a). Підключати ПІСЛЯ common.js і usb_player.js, ПЕРЕД home.js.
   Контракт — docs/web_api.md §16 (/api/usb/list, cd, play, result) і §17. Статус (media, list.version, nowPlaying)
   береться з window.usbShared (його опитує usb_player.js), власного опитування status тут немає; list НЕ опитується
   в циклі — лише після зміни list.version, дій користувача або stale.
   Імена файлів і папок — недовірений текст: лише textContent / el().
   Зовнішній інтерфейс: createUsbBrowser(mountEl) → {setActive(bool), setTitle(text)}. */
(() => {
  const addMissing = (target, src) => { Object.keys(src).forEach((k) => { if (!(k in target)) target[k] = src[k]; }); };

  addMissing(CONFIG, {
    usbRowPx: 48,             // фіксована висота рядка списку (віртуальне вікно)
    usbOverscanRows: 8,       // запас рядків над і під видимою зоною
    usbPrefetchRows: 15,      // дозавантажувати наступну сторінку, коли до кінця завантаженого лишилось менше рядків
    usbListLimitWide: 100,    // limit для широкого екрана (контракт: максимум 100; відповідь ще й обмежена ≈ 4 КБ)
    usbListLimitNarrow: 40,   // limit для телефона (контракт: за замовчуванням 40)
    usbWideMinPx: 600,
    usbListRetryMs: 2000,     // пауза після невдалого читання списку
    usbStaleRetries: 3,       // скільки разів поспіль перечитувати список з початку після stale
  });

  addMissing(ICONS, {
    folder: ['s', 'M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z'],
    music: ['s', 'M9 18V5l11-2v13M9 18a3 3 0 1 1-6 0 3 3 0 0 1 6 0zM20 16a3 3 0 1 1-6 0 3 3 0 0 1 6 0z'],
  });

  function createUsbBrowser(mount) {
    const shared = window.usbShared;
    if (!shared) { mount.hidden = true; return null; }      // usb_player.js не завантажився — як у USB16a
    const { usbApi, whyText } = shared;

    const mkBtn = (cls, label, icon) => {
      const b = el('button', { type: 'button', class: cls, 'aria-label': label, title: label });
      setIcon(b, icon);
      return b;
    };
    const ui = {
      title: el('h2', { class: 'card-title usb-br-title' }),
      up: mkBtn('btn btn-ghost btn-icon', 'На рівень вище', 'arrow-up'),
      reload: mkBtn('btn btn-ghost btn-icon', 'Оновити список', 'refresh'),
      path: el('div', { class: 'usb-path muted' }),
      note: el('p', { class: 'hint usb-br-note', role: 'status', hidden: true }),
      vwin: el('div', { class: 'usb-vwin', role: 'list' }),
      info: el('p', { class: 'hint usb-br-info', hidden: true }),
      more: el('button', { type: 'button', class: 'btn usb-more', hidden: true }, 'Ще'),
    };
    ui.view = el('div', { class: 'list usb-list', tabindex: '0', 'aria-label': 'Файли й папки флешки' }, ui.vwin);
    ui.vwin.setAttribute('aria-live', 'off');
    mount.replaceChildren(
      el('div', { class: 'card-head' }, ui.title, el('div', { class: 'usb-br-tools' }, ui.up, ui.reload)),
      ui.path, ui.note, ui.view, ui.info, ui.more);

    /* ---- Стан ---- */
    let active = false;
    let st = null;                 // останній status (з usbShared)
    let list = null;               // {version, cwd, count, dirs, truncated, next}; null = нема завантаженого списку
    let items = [];                // items[i] = {i, name, dir}; щільний масив від 0
    let reqNo = 0;                 // номер покоління списку: застарілі відповіді відкидаємо
    let loading = false;
    let loadError = '';            // текст помилки читання (порожній = нема)
    let invalid = false;           // список недійсний (вийняття / перемонтування) — чекаємо новий із status
    let nextTry = 0;               // не раніше цього моменту знову читати список
    let staleRetries = 0;
    let cmdBusy = false;           // іде cd / up / play: рядки й кнопки заблоковані
    let busyIndex = -1;
    let curKey = '', curIdx = -1;  // кеш підсвічування
    let rFirst = -1, rLast = -1;   // межі змонтованого вікна
    let rCi = -2;                  // підсвічений індекс у змонтованих рядках
    let rafPending = false;
    let unsub = null;

    const alive = () => active;
    const limit = () => (window.matchMedia('(min-width:' + CONFIG.usbWideMinPx + 'px)').matches
      ? CONFIG.usbListLimitWide : CONFIG.usbListLimitNarrow);
    const media = () => (st && st.media) || 'none';
    const modeOn = () => !!st && st.active === true;
    const ready = () => modeOn() && media() === 'mounted';
    const mayUse = () => ready() && !invalid && !!list && !cmdBusy;

    /* ---- Підсвічування поточного треку (ПРИПУЩЕННЯ через прогалину контракту) ----
       Збіг імені nowPlaying.name із файлом списку І queue.count === list.count − list.dirs
       (черга = файли тієї самої папки). Інакше — без підсвічування: краще без, ніж хибне. */
    function currentIndex() {
      const np = st && st.media === 'mounted' && st.nowPlaying;
      const q = st && st.queue;
      if (!np || !np.name || !list || !q || q.count !== list.count - list.dirs) return -1;
      const key = np.name + '|' + items.length + '|' + list.version;
      if (key === curKey) return curIdx;
      curKey = key; curIdx = -1;
      for (let i = list.dirs; i < items.length; i++) {
        if (items[i] && items[i].name === np.name) { curIdx = i; break; }
      }
      return curIdx;
    }

    /* ---- Рендер ---- */
    function noteText() {
      if (!st) return 'Завантаження стану плеєра…';
      const ms = st.mode && st.mode.state;
      if (ms === 'Entering') return 'Підключення USB…';
      if (ms === 'Exiting') return 'Вихід із режиму USB…';
      if (ms === 'Error') return 'Помилка USB-входу. Перевірте флешку або оберіть інший вхід.';
      if (!st.active) return 'Режим USB не активний.';
      if (st.media === 'none') return 'Флешку не знайдено. Вставте її — список зʼявиться сам.';
      if (st.media === 'mounting') return 'Підключення флешки…';
      if (st.media === 'fault') return 'Помилка носія. Спробуйте вийняти й вставити флешку.';
      if (invalid) return 'Список оновлюється…';
      if (list && items.length === 0 && !loading && !loadError) return 'У цій папці немає папок і файлів .mp3.';
      if (!list && loading) return 'Читання папки…';
      return '';
    }

    function infoText() {
      if (!list) return '';
      const parts = [];
      const files = Math.max(0, list.count - list.dirs);
      parts.push('Папок: ' + list.dirs + ', файлів: ' + files + '.');
      if (list.truncated) parts.push('Показано не всі записи (пристрій читає перші 1000).');
      if (loading && list.next !== null) parts.push('Завантаження…');
      if (loadError) parts.push(loadError);
      return parts.join(' ');
    }

    function buildItem(i, cur) {
      const it = items[i];
      const btn = el('button', { type: 'button', class: 'row usb-row', 'data-i': i, 'data-dir': it.dir ? '1' : '0',
        'aria-label': (it.dir ? 'Папка ' : 'Файл ') + it.name });
      btn.append(icon(it.dir ? 'folder' : 'music'), el('span', { class: 'name' }, it.name));
      if (cur) btn.setAttribute('aria-current', 'true');
      if (cmdBusy || invalid) btn.disabled = true;
      if (cmdBusy && i === busyIndex) btn.setAttribute('aria-busy', 'true');
      const li = el('div', { class: 'usb-item', role: 'listitem' }, btn);
      li.style.height = CONFIG.usbRowPx + 'px';
      return li;
    }

    /* Віртуальне вікно: у DOM лише видимі рядки ± запас, решта — padding-розпірки (фіксована висота рядка). */
    function renderRows(force) {
      const total = items.length, h = CONFIG.usbRowPx;
      const vh = ui.view.clientHeight || 400;
      const top = ui.view.scrollTop;
      const first = Math.max(0, Math.floor(top / h) - CONFIG.usbOverscanRows);
      const last = Math.min(total, Math.ceil((top + vh) / h) + CONFIG.usbOverscanRows);
      if (!force && first === rFirst && last === rLast) return;
      rFirst = first; rLast = last;
      const ci = currentIndex();
      rCi = ci;
      const focusedI = ui.vwin.contains(document.activeElement) ? document.activeElement.getAttribute('data-i') : null;
      const frag = document.createDocumentFragment();
      for (let i = first; i < last; i++) frag.append(buildItem(i, i === ci));
      ui.vwin.replaceChildren(frag);
      ui.vwin.style.paddingTop = first * h + 'px';
      ui.vwin.style.paddingBottom = (total - last) * h + 'px';
      if (focusedI !== null) {
        const b = ui.vwin.querySelector('button[data-i="' + focusedI + '"]');
        if (b && !b.disabled) b.focus({ preventScroll: true });
      }
      maybePrefetch();
    }

    function render(rowsToo) {
      const nt = noteText();
      ui.note.hidden = !nt;
      setText(ui.note, nt);
      const cwd = list ? list.cwd : (st && st.cwd) || null;
      setText(ui.path, ready() && cwd ? (cwd === '/' ? 'Корінь флешки' : cwd) : '');
      setAttr(ui.path, 'title', cwd || '');
      ui.up.disabled = !mayUse() || !list || list.cwd === '/';
      ui.reload.disabled = !ready() || cmdBusy || loading;
      const it = infoText();
      ui.info.hidden = !it;
      setText(ui.info, it);
      ui.more.hidden = !(list && list.next !== null && !loading && loadError !== '');
      ui.view.hidden = !list || !ready();
      setAttr(ui.view, 'aria-busy', loading || cmdBusy);
      if (rowsToo !== false) renderRows(true);
    }

    function scheduleRows() {
      if (rafPending) return;
      rafPending = true;
      requestAnimationFrame(() => { rafPending = false; if (active) renderRows(false); });
    }

    function clearList() {
      reqNo++;
      list = null; items = []; loading = false; loadError = ''; invalid = false;
      curKey = ''; curIdx = -1; rFirst = rLast = -1; staleRetries = 0;
    }

    /* ---- Читання списку ---- */
    function maybePrefetch() {
      if (!list || list.next === null || loading || invalid || loadError || cmdBusy) return;
      if (rLast >= items.length - CONFIG.usbPrefetchRows) fetchPage();
    }

    function loadFirst() {
      reqNo++;
      items = []; curKey = ''; curIdx = -1; rFirst = rLast = -1;
      list = null; loading = false; loadError = ''; ui.view.scrollTop = 0;
      return fetchPage(true);
    }

    async function fetchPage(first) {
      if (loading || !active) return;
      if (!first && (!list || list.next === null)) return;
      const my = reqNo;
      loading = true; render();
      try {
        const offset = first ? 0 : list.next;
        const r = await usbApi.list(offset, limit(), first ? null : list.version);
        if (my !== reqNo || !active) return;
        if (!first && r.version !== list.version) { loading = false; await loadFirst(); return; }
        if (first) list = { version: r.version, cwd: r.cwd, count: r.count, dirs: r.dirs, truncated: !!r.truncated, next: null };
        (r.items || []).forEach((it) => { items[it.i] = { i: it.i, name: String(it.name), dir: !!it.dir }; });
        const nx = typeof r.nextOffset === 'number' ? r.nextOffset : null;
        list.next = nx !== null && nx > offset && nx < r.count ? nx : null;   // нема просування — зупиняємось, а не крутимо цикл
        invalid = false; loadError = ''; staleRetries = 0; nextTry = 0;
        for (let i = 0; i < items.length; i++) if (!items[i]) { items.length = i; list.next = null; break; }   // дірок не лишаємо
      } catch (e) {
        if (my !== reqNo || !active) return;
        handleListError(e);
      } finally {
        if (my === reqNo) { loading = false; if (active) { render(); sync(); } }
      }
    }

    function handleListError(e) {
      const code = e instanceof ApiError ? e.code : '';
      nextTry = Date.now() + CONFIG.usbListRetryMs;
      if (code === 'stale') {                                  // список змінився: тихо читаємо з початку
        if (staleRetries++ < CONFIG.usbStaleRetries) { loading = false; loadFirst(); return; }
        loadError = whyText('stale');
      } else if (code === 'list_invalid') {
        invalid = true; items = []; list = null;               // новий список прийде зі status (інша list.version)
      } else if (code === 'no_media' || code === 'usb_mode_off') {
        clearList();                                           // пояснення дасть status
      } else if (e instanceof ApiError && e.status === 404) {
        loadError = 'USB-плеєр недоступний у цій збірці прошивки.';
      } else {
        loadError = errorText(e);
      }
    }

    /* Звіряє status із завантаженим списком: інша list.version → перечитати (зміна папки на пристрої, вставлення, відновлення). */
    function sync() {
      if (!active) return;
      if (!ready()) {
        if (list || loading || invalid || loadError) clearList();
        render();
        return;
      }
      const sl = st.list || {};
      if (!sl.valid) {                                         // вийняття / перемонтування: старий список недійсний
        if (list) { clearList(); }
        invalid = true; render();
        return;
      }
      invalid = false;
      if (loading || cmdBusy || Date.now() < nextTry) { render(false); return; }
      if (!list || sl.version !== list.version) loadFirst();
      else { render(false); if (currentIndex() !== rCi) renderRows(true); }   // лише підсвічування, без перебудови щосекунди
    }

    /* ---- Команди ---- */
    async function runCmd(index, call, reloadOnOk) {
      if (cmdBusy || !mayUse()) return;
      cmdBusy = true; busyIndex = index; render();
      let reload = false;
      try {
        const r = await call();
        const req = await shared.waitReq(r.reqId, alive);
        if (!active) return;
        if (req === null) { toast('Пристрій не підтвердив виконання вчасно.', 'error'); reload = true; }
        else if (req.ok === false) {
          if (req.why !== 'stale' && req.why !== 'list_invalid') toast(whyText(req.why), 'error');
          reload = true;
        } else reload = reloadOnOk || req.ok === null;   // cd / up → новий список; play → без перечитування
      } catch (e) {
        if (e instanceof ApiError && (e.code === 'stale' || e.code === 'list_invalid')) reload = true;   // тихо
        else { toast(errorText(e), 'error'); reload = e instanceof ApiError && e.status === 409; }
      } finally {
        cmdBusy = false; busyIndex = -1;
      }
      if (!active) return;
      if (reload) { nextTry = 0; await loadFirst(); } else render();
    }

    const doCd = (i) => runCmd(i, () => usbApi.cd({ index: i, version: list.version }), true);
    const doUp = () => runCmd(-1, () => usbApi.cd({ up: true }), true);
    const doPlay = (i) => runCmd(i, () => usbApi.play({ index: i, version: list.version }), false);

    ui.vwin.addEventListener('click', (ev) => {
      const b = ev.target.closest('button[data-i]');
      if (!b || b.disabled || !mayUse()) return;
      const i = Number(b.getAttribute('data-i'));
      const it = items[i];
      if (!it) return;
      if (it.dir) doCd(i); else doPlay(i);
    });
    ui.up.addEventListener('click', doUp);
    ui.reload.addEventListener('click', () => { if (ready() && !cmdBusy) { nextTry = 0; loadFirst(); } });
    ui.more.addEventListener('click', () => { loadError = ''; nextTry = 0; fetchPage(false); });
    ui.view.addEventListener('scroll', scheduleRows, { passive: true });
    let rsz = null;
    window.addEventListener('resize', () => { clearTimeout(rsz); rsz = setTimeout(() => { if (active) renderRows(true); }, CONFIG.resizeDebounceMs || 150); });

    function onStatus(s) {
      if (!active) return;
      st = s;
      sync();
    }

    render();

    return {
      /* Вмикається з render() Головної (активний вхід USB і не standby): скидає стан при вимкненні. */
      setActive(on) {
        if (on === active) return;
        active = on;
        if (on) {
          unsub = shared.onStatus(onStatus);
          st = shared.getStatus();
          sync();
        } else {
          if (unsub) { unsub(); unsub = null; }
          clearList();
          st = null; cmdBusy = false; busyIndex = -1; nextTry = 0;
          render();
        }
      },
      setTitle(text) { setText(ui.title, text || ''); },
    };
  }

  window.createUsbBrowser = createUsbBrowser;
})();
