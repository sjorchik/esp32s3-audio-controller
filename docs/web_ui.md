# Веб-інтерфейс: каркас фронтенду (Prompt 19, доповнено Prompt 20, 21, 22, 23, 24 і 25b)

Документ для наступних промптів (P25+). Нові сторінки додаються **лише змінами в `webui/`** — без правок C++.
Контракт API — `docs/web_api.md`.

## 1. Як це працює

```
webui/**  --tools/build_web.py-->  src/net/web_assets_gen.cpp  --компіляція-->  gzip-масиви у flash прошивки
```

- Вихідники — `webui/` (читабельні, у git). Згенерований `src/net/web_assets_gen.cpp` у `.gitignore`.
- `platformio.ini`: `extra_scripts = pre:tools/build_web.py` — скрипт виконується перед КОЖНОЮ збіркою. Вручну: `python tools/build_web.py` (друкує таблицю «файл / сирий / gzip»). Файл переписується лише якщо вміст змінився.
- Для кожного `x.html` автоматично є псевдонім `/x`; `/` = `/index.html`. Нові `.html/.css/.js/.svg/.json/.ico/.png/.txt` у `webui/` підхоплюються самі (інше розширення = помилка збірки).
- Сторінки НЕ в LittleFS: `uploadfs` їх не чіпає, і вони оновлюються з прошивкою (у т.ч. через OTA). Після зміни `webui/` потрібна перепрошивка.
- Режим AP (captive portal) має власний сервер у `wifi_manager.cpp`; вбудовані сторінки віддає лише сервер STA.

## 2. Структура `webui/`

| Файл | Призначення |
|---|---|
| `index.html` | сторінка «Головна» |
| `css/app.css` | усі спільні стилі й токени |
| `js/common.js` | спільний код (підключати ПЕРШИМ) |
| `js/nav.js` | список сторінок + `renderNav()` (підключати ДРУГИМ) |
| `js/home.js` | логіка Головної |
| `stations.html`, `js/stations.js` | сторінка «Станції» (P20): список, форма, переміщення, імпорт / експорт; рівень станції `levelDb` — поле форми, «Застосувати», бейдж у списку (P25b) |
| `audio.html`, `js/audio.js` | сторінка «Аудіо» (P21): гучність / мʼют, gain і вхід, бас / дискант / баланс, loudness, скидання тембру |
| `ir.html`, `js/ir.js` | сторінка «Пульт» (P22, `/ir`): мапа дій по групах, навчання, очищення, імпорт / експорт; зразок довгої операції з опитуванням статусу |
| `settings.html`, `js/settings.js` | сторінка «Налаштування» (P23, `/settings`): тип процесора, назви входів, яскравість і орієнтація дисплея, Wi-Fi (лише читання); кожне поле зберігається окремим `POST /api/settings` |
| `system.html`, `js/system.js` | сторінка «Система» (P23, `/system`): інформація з `/api/system` (за кнопкою), картка «Оновлення прошивки» (OTA, P24), сервісні дії з підтвердженням |
| `favicon.svg` | іконка |

Скрипти — класичні (`<script src>`, не модулі): `const`/функції верхнього рівня `common.js` і `nav.js` видимі в решті скриптів.

## 3. Як додати сторінку

1. Створіть `webui/<id>.html` за шаблоном:

```html
<!doctype html>
<html lang="uk">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark light">
<title>Станції</title>
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
<link rel="stylesheet" href="/css/app.css">
</head>
<body>
<header class="topbar">
  <div class="topbar-row">
    <h1 class="brand" id="brand"></h1>
    <span id="wifi" class="wifi" role="img"></span>
    <button id="btn-power" class="btn btn-icon" type="button" aria-pressed="false" aria-label="Живлення" data-icon="power"></button>
  </div>
  <nav id="nav" class="nav" aria-label="Розділи"></nav>
</header>
<main class="page">
  <div id="banners" class="banners"></div>
  <!-- вміст сторінки -->
</main>
<script src="/js/common.js"></script>
<script src="/js/nav.js"></script>
<script src="/js/stations.js"></script>
</body>
</html>
```

   Кнопка живлення й індикатор Wi-Fi в шапці — частина каркаса; їх поведінку зараз реалізує `home.js`. Для інших сторінок або винесіть цю логіку в `common.js`, або не показуйте елементи. Сторінка «Станції» лишає лише `#brand`, `#nav` і `#wifi` (індикатор малює `renderWifi(node, st.wifi)` зі `status`); кнопки живлення там немає.
2. Створіть `webui/js/<id>.js`; на початку викличте `initShell('<id>')`.
3. У `nav.js` переведіть `enabled: false` → `true` для цієї сторінки. `href` має збігатися з файлом (`/stations.html`).
4. `python tools/build_web.py` → збірка → прошивка.

## 4. CSS-компоненти (`app.css`)

Токени — CSS-змінні в `:root` (темна за замовчуванням, світла через `prefers-color-scheme`): кольори `--bg --surface --surface-2 --line --text --muted --accent --accent-ink --ok --err --info`; відступи `--sp-1..--sp-6` (4/8/12/16/24/32 px); радіуси `--r-s --r-m --r-l`; `--tap` (44 px). Кольорів у компонентах не хардкодьте.

| Клас | Призначення |
|---|---|
| `.page` | контейнер вмісту (max 960 px) |
| `.home-grid` + `.col` | одна колонка на телефоні, дві ≥ 860 px |
| `.card`, `.card-head`, `.card-title` | картка, її шапка, заголовок |
| `fieldset.group` | група елементів; `disabled` на fieldset вимикає всі контролі всередині |
| `.btn` | кнопка (≥ 44×44) |
| `.btn-primary` / `.btn-ghost` / `.btn-danger` | варіанти |
| `.btn-icon` / `.btn-lg` / `.btn-xl` | квадратна / велика / кругла велика; `aria-pressed="true"` підсвічує кнопку |
| `.slider` | `<input type="range">` |
| `.badge` + `.badge-ok/.badge-warn/.badge-err` | бейдж статусу (без модифікатора — нейтральний) |
| `.banner` + `.banner-warn/.banner-error`, `.progress` | банер (створюйте через `setBanner`) |
| `.toast`, `.toasts` | створюються через `toast()` |
| `.dialog` | створюється через `confirmDialog()` |
| `.list`, `.row` (`.idx`, `.name`, `aria-current="true"`) | прокручуваний список кнопок |
| `.seg` (кнопки з `role="radio"`, `aria-checked`) | сегментований перемикач |
| `.form`, `.field` (`<label>` + `.input`/`.select`), `.field-error`, `.switch` | форми |
| `.table-wrap` + `.table` | таблиця (обгортка дає горизонтальну прокрутку) |
| `.marquee` (> `<span>`) | біжучий рядок; керується `home.js: setMarquee` |
| `.toolbar` | ряд кнопок, що переноситься (кнопки розтягуються) |
| `.param` > `.param-head` (назва + `output.param-val`) + `.param-row` (−, `.slider`, +, `.btn-zero`) | рядок параметра з повзунком (P21); `.slider-c` додає позначку центру |
| `.station-list` > `li.station` | список-картки станцій: `.station-main` (`.idx`, `.station-text` > `.station-name` + `.station-url`), `.station-actions`; `aria-current="true"` = поточна |
| `.dialog.dialog-form` | діалог із формою (`h2`, `.form` > `.field`, `.dialog-actions`) |
| `.badge.station-level` | приглушений бейдж рівня в `.station-main` (P25b); без крапки, ховається < 480 px |
| `.level-val` | підпис значення («−6 дБ») праворуч від повзунка у `.param-row` форми станції (P25b) |
| `fieldset.group.is-busy` | разом із `disabled`: «зайнято» без сильного затемнення (на час запиту) |
| `.ir-learn`, `.ir-target` | панель навчання кнопки пульта (P22): рамка акценту, велика назва цілі; кнопки — `.dialog-actions`. Рядки дій на сторінці «Пульт» — це `ul.station-list` / `li.station` (див. вище), групи — `section.card` |
| `.kv` | таблиця «параметр — значення» (`table.table.kv`): перша колонка приглушена, значення переносяться по символах |
| `.danger-zone` | картка з небезпечними діями (рамка `--err`); дії — `ul.station-list` / `li.station` + `.btn-danger` для незворотних |
| `.ota-file`, `.ota-name`, `.ota-pick`, `.ota-prog` | картка OTA (P24): рядок вибору файлу, блок передачі з `.progress` (12 px) і кнопкою «Скасувати» (поза `fieldset`) |
| `.wait`, `.wait-box`, `.spinner` | екран очікування перезапуску (створює `waitScreen`); `p` у ньому зберігає переноси рядків |
| `.stack`, `.row-flex`, `.spacer`, `.muted`, `.hint`, `.num`, `.visually-hidden` | службові |

Правила: цілі натискання ≥ 44 px; фокус видно через `:focus-visible`; `[hidden]` завжди ховає елемент.

## 5. Публічні функції `common.js`

Усі глобальні. Єдиний об'єкт налаштувань — `CONFIG` на початку файлу (інтервали, тайм-аути, пороги; додавайте нові ключі туди, а не «магічні» числа в код сторінок).

| Сигнатура | Опис |
|---|---|
| `api.get(path, timeoutMs?) → Promise<json>` | GET; JSON-відповідь |
| `api.post(path, body?, timeoutMs?) → Promise<json>` | POST із JSON (`{}` за замовчуванням) |
| `api.base() → string` | базова адреса (порожня в продакшені) |
| `class ApiError { status, code, reason, data }` | кидається при будь-якій помилці. `code` = `error` з тіла (або `network`/`timeout`/`bad_response`), `reason` = `reason` з тіла, `data` — все тіло (`field`, `min`, `max`…) |
| `errorText(e) → string` | українське повідомлення; пошук у `REASONS` за `reason`, потім за `code` |
| `REASONS` | словник код → текст. Нові коди додавайте сюди |
| `toast(text, kind?)` | `kind`: `'ok'` / `'error'` / інше = info; `aria-live` |
| `confirmDialog(text, {okText, cancelText, danger}?) → Promise<boolean>` | модальний діалог (`<dialog>`) |
| `setBanner(id, text\|null, kind?, {progress}?)` | показати/оновити/прибрати банер у `#banners`; `kind`: `'info'`/`'warn'`/`'error'`; `progress` 0..100 малює смугу |
| `createPoller(fn, {intervalMs}?) → {start, stop, now}` | опитування (див. §6) |
| `connection.offline`, `onConnectionChange(fn(offline))`, `setOffline(bool)` | стан звʼязку; банер «Немає звʼязку» керується пулером |
| `el(tag, attrs?, ...children) → Element` | будівник DOM. Рядки-діти — ЗАВЖДИ текст; `onclick: fn` додає слухач; `false/null` пропускаються |
| `$(sel, root?)` | `querySelector` |
| `clamp(v, lo, hi)`, `setText(node, text)`, `setAttr(node, name, value)` | `setText`/`setAttr` пишуть лише при зміні |
| `icon(name) → SVGElement`, `setIcon(node, name)`, `hydrateIcons(root?)` | іконки: `power play pause stop prev next volume mute minus plus refresh search edit trash arrow-up arrow-down move upload download check close reset`; `hydrateIcons` обробляє `[data-icon]` |
| `api.put(path, body?, timeoutMs?)`, `api.del(path, timeoutMs?)` | PUT із JSON / DELETE без тіла (P20) |
| `api.send(method, path, body, contentType, timeoutMs?) → Promise<json>` | СИРЕ тіло (`File`/`Blob`/рядок) із заданим `Content-Type`; відповідь — JSON. Для імпорту |
| `api.blob(path, timeoutMs?) → Promise<Blob>` | GET файлу (експорт). Помилки — як у решти `api.*` (`ApiError`) |
| `downloadBlob(blob, name)`, `fileStamp() → 'YYYY-MM-DD'` | зберегти Blob як файл; дата для імені файлу |
| `byteLength(s)`, `formatBytes(n)`, `debounce(fn, ms)` | довжина в байтах UTF-8 (сервер рахує байти); «12.3 КБ» / «7.0 МБ» (не число → «—»); відкладений виклик |
| `formatUptime(ms)` | «3 діб 4 год 5 хв»; менше хвилини — «42 с» |
| `waitScreen(title, text, spin)` → `{set(title, text, spin, button?)}` | повноекранне очікування (`.wait`); сторінка під ним стає `inert`. Закрити не можна — лише `location.reload()` |
| `waitForDevice({delayMs, timeoutMs, intervalMs, onTick})` → `Promise<bool>` | опитує `/api/status`, поки пристрій не відповість після перезапуску: була недоступність **або** `uptimeMs` менший за час очікування (швидкий reboot) |
| `rebootAndWait(path, body, before)` | POST (`body === undefined` → без тіла) → `waitScreen` → `waitForDevice` → `location.reload()`; таймаут → кнопка «Оновити сторінку». Помилку запиту кидає (toast — у викликача); `before()` зупиняє опитування сторінки |
| `rssiToBars(rssi) → 0..4`, `renderWifi(node, status.wifi)` | індикатор Wi-Fi |
| `createSlider(input, {send, onInput, onFail}) → {held, hold, release, send}` | повзунок з тротлінгом і утриманням (P21), див. §5.2 |
| `fmtSigned(v) → string` | `+3` / `0` / `−3` (P21) |
| `formatEta(sec) → string` | залишок часу: «8 с», «1 хв 5 с»; не число → «—» (P24) |
| `initShell(activeId)` | заголовок, іконки, навігація, dev-банер |

Нова в P22: `CONFIG.irLearnPollMs` (400), `irStatusPollMs` (2000), `irImportMaxBytes` (8192); у `REASONS` — `not_learnable`, `unknown_action`, `invalid_action`, `no_code_for_action`, `not_in_conflict`. Нових іконок і функцій немає.

Нова в P23: `CONFIG.settingsPollMs` (2000), `savedMs` (1800), `inputNameMaxBytes` (31), `processorInputs` (`Tda7318`: 4, `Pt2313l`: 3), `rebootPollMs` (1500), `rebootPingMs` (2500), `rebootTimeoutMs` (60000), `apSsid`, `apAddress`, `mdnsHost`; у `REASONS` — `invalid_name`, `expected_array_of_4`, `empty_request`. `errorText` бере `min` / `max` з `errors[0]` для `POST /api/settings`.

Нова в P24: `CONFIG.otaMinBytes` (262144), `otaMaxBytes` (5767168, запасний ліміт), `otaMagic` (0xE9), `otaUploadTimeoutMs` (300000), `otaRebootDelayMs` (1500), `otaRebootTimeoutMs` (90000); у `REASONS` — `incomplete`, `begin_failed`, `write_failed`, `no_ota_partition`, `ota_done_restarting`; `formatEta`. Іконок нових немає.

Нова в P25b: `CONFIG.stationLevelMinDb` (−24), `stationLevelMaxDb` (0), `stationLevelDefaultDb` (−6); у `REASONS` — `level_invalid`, `level_out_of_range`; `errorText` додає межі `min…max` до `level_out_of_range` так само, як до `out_of_range`. Нових іконок і функцій немає.

**Безпека:** усе, що походить від пристрою або потоку (назви станцій, ICY-заголовок, ssid), вставляйте ТІЛЬКИ через `textContent` / `el()` / `setText`. `innerHTML` не використовується ніде і не повинен.

Приклад:

```js
try {
  const r = await api.post('/api/volume', { step: 5 });
  toast('Гучність: ' + r.state.volume, 'ok');
} catch (e) {
  toast(errorText(e), 'error');      // напр. «Пристрій у режимі очікування.»
}
if (await confirmDialog('Перезапустити пристрій?', { okText: 'Перезапустити', danger: true })) { /* … */ }
```

`nav.js`: `NAV_PAGES` (`{id, href, title, enabled}`), `pageById(id)`, `renderNav(activeId)`.

### 5.1. Форми, діалоги, імпорт / експорт (P20)

Зразок — `js/stations.js`.

- **Діалог із формою.** `<dialog class="dialog dialog-form">` > `<form novalidate>`: `h2`, `div.form` > `div.field` (`label` + `input.input` + `div.field-error[role=alert][hidden]`), `div.dialog-actions` (Скасувати + `submit`). Відкривати `showModal()`, у `close` — `dlg.remove()`. На час запиту вимикайте кнопки й блокуйте `cancel` (Esc). `openForm({title, submit, fields, validate, save, level?})` у `stations.js` — готова обгортка (поки локальна; винесіть у `common.js`, коли знадобиться другій сторінці).
- **Помилки полів.** Сервер повертає `field` у тілі (`name`, `url`…) → `ApiError.data.field`; показуйте `errorText(e)` у `.field-error` цього поля, решту — `toast`. Клієнтська валідація дзеркалить серверну, а числа — у `CONFIG` (`stationNameMaxBytes`, `stationUrlMaxBytes`). Довжини — **у байтах** (`byteLength`): кирилична літера = 2 байти.
- **Позиційні списки.** Індекси зсуваються після видалення / переміщення / імпорту: після КОЖНОЇ зміни (навіть невдалої) перечитайте і список, і `/api/status` (`write()` у `stations.js`).
- **Імпорт.** Файл → `api.send('POST', '/api/stations/import?format=json|m3u|pls', file, contentType, CONFIG.importTimeoutMs)`. Формат — за розширенням (`.m3u8` → `m3u`). Перевірте розмір (`CONFIG.importMaxBytes`) до відправки. Імпорт ЗАМІНЯЄ весь список — спершу `confirmDialog`. Режиму «додати» контракт не має. Результат — `{count, replaced}`; помилка — `ApiError` із `reason` (= `lastImportError`).
- **Експорт.** `const b = await api.blob('/api/stations/export'); downloadBlob(b, 'stations-' + fileStamp() + '.json')` — працює і в dev-режимі (`?device=`), бо запит іде через `api`, а не посиланням.
- **Блокування.** `<fieldset disabled>` навколо всього інтерактивного вмісту + `is-busy`, поки триває запит; у режимах OTA / IR / WifiSetup / офлайн — той самий `disabled` і банер.
- **Рівень станції (P25b).** `openForm(..., level: {value, apply?})` додає після текстових полів блок «Рівень, дБ»: `.field` > `label` + `.param-row` (−, `.slider` `min`/`max`/`step=1` з `CONFIG.stationLevel*`, +, `output.level-val` зі `fmtSigned` і «дБ») + `.hint` + `.field-error`. Значення йде в `vals.levelDb` (число) і з `POST` / `PUT` відправляється ЗАВЖДИ; для нової станції — `stationLevelDefaultDb`, для редагування — `levelDb` рядка (немає в старій відповіді → дефолт). Помилка з `field: "levelDb"` потрапляє під повзунок тим самим механізмом, що й `name` / `url`.
- **«Застосувати» (P25b, лише редагування).** `level.apply(db)` шле `PUT /api/stations/{index}` з ЗБЕРЕЖЕНИМИ `name` / `url` рядка (контракт вимагає обидва; значення форми не беруться, щоб правка URL не розʼїхалась зі станцією, що грає, і не зберігалась непомітно) та `levelDb` форми. Діалог не закривається; на час запиту «Зберегти», «Скасувати» й Esc вимкнені; успіх → бейдж `.badge-ok` «Застосовано» на `CONFIG.savedMs` (ховається при русі повзунка), список перечитується у тлі (`reload()` без `guarded`). Помилка — під повзунком. «Застосувати» вже записало рівень: «Скасувати» його не відкочує. Рівень звучить наживо, лише якщо станція з цим індексом зараз грає.
- **Список (P25b).** `GET /api/stations` → `levelDb` → `.badge.station-level` («−6 дБ»); поле відсутнє — бейджа немає.

### 5.2. Повзунки параметрів (P21)

Зразок — `js/audio.js` (гучність, бас, дискант, баланс).

- `createSlider(input, {send, onInput, onFail})`: відправка не частіше `CONFIG.volumeSendMinMs`, остаточне значення на `change`, утримання `CONFIG.volumeHoldMs` після відпускання. `send(v)` — `async`, кидає `ApiError`; при відмові toast + `onFail()` (перемалюйте з `st`). Поки `ctl.held()`, опитування не перезаписує повзунок. Кнопки −/+/«0»: виставте `input.value`, оновіть підпис, викличте `ctl.send(v)`.
- Межі беріть з `capabilities` у кожному рендері: `min` / `max` ставте ПЕРЕД `value`. Функції, яких чип не підтримує, ховайте (`hidden`).
- Тембр / баланс / loudness — `POST /api/settings` з одним полем; успіх → оновіть `st[key]`. HTTP 502 (`i2c_failed`) `errorText` перетворює на «Аудіопроцесор не відповів».
- Нейтральні значення — `CONFIG.audioNeutral` (контракт їх не наводить), `CONFIG.gainStepDb` — крок gain за типом чипа.
- Оптимістичні gain / вхід: після кліку `refreshStatus` ~`volumeHoldMs` не перезаписує їх.
- Гейти: у standby контракт відхиляє гучність / мʼют / gain / вхід (`409 standby`), тембр / баланс / loudness не гейтяться — лишаються активними. OTA / IR / WifiSetup / офлайн блокують усе.

### 5.3. Довга операція з опитуванням статусу (P22)

Зразок — `js/ir.js` (навчання кнопки пульта).

- **Два пулери.** Загальний `/api/status` (`CONFIG.irStatusPollMs`) працює завжди; пулер операції (`/api/ir/learn/status`, `CONFIG.irLearnPollMs`) запускається лише поки операція триває (`session` — запущена з цієї сторінки, або `mode === 'IrLearn'` — запущена на пристрої) і зупинюється в `render()`, коли потреба зникла.
- **Панель операції ПОЗА `fieldset.group`.** Решта сторінки під час навчання `disabled` (пристрій відповідає `409 learning_active` / `ir_learn_active`), а «Скасувати» лишається доступною.
- **Результат Success / Timeout лишається в статусі** до `cancel` або нового навчання, тому його обробляють один раз (прапорець `finalDone`) і не скидають зайвим `cancel` — пристрій сам показує результат ~1.5 с і повертає режим. Стан `Idle` без сесії = навчання завершено.
- **Навчання з пристрою.** `mode === 'IrLearn'` без локальної сесії: банер + та сама панель (із «Скасувати» і, при `Conflict`, «Перезаписати»). Стару `Success` / `Idle` при цьому не показуємо.
- **Таймаут.** Пристрій сам завершує навчання статусом `Timeout`, тому при закритті сторінки скасування не надсилається.
- **Групи за словником.** Назви / групи — словник у `ir.js` (ключ — імʼя дії з API); невідома дія → група «Інше» із сирим імʼям.
- **Імпорт мапи.** `api.send('POST', '/api/ir/map/import', текст файлу, 'application/json', …)`; перед відправкою — розмір ≤ `CONFIG.irImportMaxBytes`, JSON-масив, `confirmDialog`. Експорт — `api.blob('/api/ir/map')` → `ir-map-YYYY-MM-DD.json`.

### 5.4. Налаштування та сервісні дії (P23)

- **Автозбереження.** Кожен елемент (select, перемикач, повзунок) шле власний `POST /api/settings` з одним полем і показує бейдж `.badge-ok` «Збережено» на `CONFIG.savedMs`. Помилка → toast і відкат елемента.
- **Назви входів.** Масив завжди з 4 елементів: `null` = не змінювати. Клієнтська перевірка дзеркалить сервер (1…31 байт UTF-8, без керівних символів, `byteLength`); помилка сервера з `errors[i].index` показується під відповідним полем. Кількість полів залежить від вибраного чипа (`CONFIG.processorInputs`).
- **Тип процесора** діє після перезапуску: збережений тип (`/api/settings`) порівнюється з працюючим (`status.processor.type`); якщо різні — банер із кнопкою «Перезапустити зараз».
- **Деструктивні дії.** `confirmDialog` (що саме зміниться / не зміниться) → кнопки блокуються (`busy`) → запит. Перезапуск і скидання налаштувань — `rebootAndWait`; скидання Wi-Fi — `waitScreen` з інструкцією без очікування відповіді (мережа зміниться).
- **Важкий ендпоінт.** `/api/system` не опитується: читається при відкритті та за кнопкою «Оновити».
- **Блокування.** Лише `offline` / ще немає даних; режими `OtaUpdate` / `IrLearn` / `WifiSetup` на «Налаштуваннях» дають попередження без блокування (контракт їх не забороняє). На «Системі» під час OTA (з пристрою чи іншого клієнта) вимкнені сервісні дії та старт нового оновлення (див. §5.5).

### 5.5. OTA-оновлення прошивки (P24)

Зразок — картка «Оновлення прошивки» в `system.html` / `js/system.js`. Контракт — `web_api.md` §8.4.

- **Запит.** Сире тіло `.bin`, `Content-Type: application/octet-stream`, без multipart; `Content-Length` ставить браузер (chunked пристрій не підтримує). Використовується `XMLHttpRequest` (`xhr.upload.onprogress` дає прогрес передачі; `fetch` — ні). URL — `api.base() + '/api/ota'`, тож працює і в dev-режимі. Запит **поза чергою `api.*`**, тайм-аут `CONFIG.otaUploadTimeoutMs`.
- **Підготовка до старту.** `confirmDialog` (danger) → `uploading = true` → `poller.stop()` → `await api.get('/api/status')`: цей запит стає в чергу ПІСЛЯ вже відправлених, тож черга спорожніла; заодно режим перевіряється свіжим статусом (`OtaUpdate` / `IrLearn` / `WifiSetup` → відмова, `blockReason`).
- **Блокування.** Поки `uploading`: усі `fieldset` сторінки `disabled`, кнопки «Оновити» (інформація) і вибору файлу вимкнені, опитувань немає, `beforeunload` просить підтвердити закриття / перехід. Блок прогресу стоїть ПОЗА `fieldset`, щоб «Скасувати» лишалась доступною (як панель навчання в §5.3).
- **Перевірки файлу ДО відправки** (`checkImage`): розширення `.bin`; розмір > 0, ≥ `otaMinBytes` (відсікає `bootloader.bin`, `partitions.bin`), ≤ `ota.maxImageBytes` зі `/api/system` (запасно `otaMaxBytes`); перший байт = `otaMagic` (`file.slice(0, 1)`). Перевірка повторюється перед діалогом.
- **Прогрес.** Смуга + «N% · відправлено з усього · швидкість (середня) · залишок». Коли `upload.onload` спрацював (`sent`) — смуга 100 %, текст «пристрій перевіряє образ…», «Скасувати» ховається (після повної передачі пристрій уже верифікує й перезапускається). Прогрес ЗАПИСУ на пристрої не опитується (опитування зупинені); показує лише передачу.
- **Скасування.** `xhr.abort()` → пристрій бачить розрив зʼєднання й повертає режим і звук (за контрактом: «на помилці звук і режим відновлюються»); активний слот не змінюється, бо перемикання слота відбувається лише після успішної перевірки. Після скасування й після помилки опитування `/api/status` відновлюється.
- **Помилки.** Відповідь із `ok:false` → `ApiError(status, data, data.error)` → `errorText` (спершу `reason`, потім `error`); результат — банер `ota-result` (error / info). Обрив `network` / `timeout` ДО кінця передачі → «стан пристрою невідомий». Обрив `network` ПІСЛЯ `upload.onload` (відповідь могла загубитись при перезапуску) → та сама гілка очікування, що й після успіху.
- **Після успіху** (`200 {ok, restarting}`): `waitScreen` → `waitForDevice({delayMs: otaRebootDelayMs, timeoutMs: otaRebootTimeoutMs})` → `GET /api/system` → підсумок на екрані очікування: «Було» (останній `/api/system` до оновлення) / «Стало», час роботи, причина скидання; кнопка «Оновити сторінку». Висновку «успіх / невдача» за версією немає: `buildDate` — час компіляції окремого файлу і може не змінюватись. `restarting:false` → прохання перезапустити живленням (`/api/system/reboot` відхиляється, доки OTA не завершено). Таймаут → «Пристрій не відповідає… можливо, потрібна перепрошивка по USB».
- **Безпека.** Імʼя файлу, версія, дата збірки — лише через `setText` / `textContent`.

## 6. Шаблон опитування

```js
let st = null;
async function refresh() {
  st = await api.get('/api/status');   // кинути помилку = невдача
  render();
}
const poller = createPoller(refresh, { intervalMs: CONFIG.pollStatusMs });
poller.start();
// після команди: poller.now();
```

- Запити НІКОЛИ не йдуть паралельно: усі `api.*` виконуються однією послідовною чергою, пулер починає наступний цикл лише після завершення попереднього.
- На прихованій вкладці (`document.hidden`) опитування призупиняється й відновлюється одразу при поверненні.
- Помилка → інтервал ×`pollBackoffFactor` (до `pollBackoffMaxMs`); після `offlineAfterFailures` збоїв ЗВʼЯЗКУ (`network`/`timeout`) з'являється банер «Немає звʼязку з пристроєм». Інші помилки (4xx/5xx) банер не вмикають.
- Окремі пулери для `/api/ir/learn/status` (300–500 мс) і `/api/system` (≥ 5 с) створюйте тим самим `createPoller(fn, {intervalMs})`; не запускайте кілька одночасно, якщо в цьому немає потреби.
- Команди керування повертають `{ok, state}` — використайте для миттєвого оновлення UI, потім `poller.now()`.

## 7. Словник `reason`

Повний словник — об'єкт `REASONS` у `common.js` (коди керування: `standby`, `ota_in_progress`, `ir_learn_active`, `not_radio_input`, `wifi_setup`, `no_stations`, `input_unavailable`, `station_out_of_range`, `volume_out_of_range`, `gain_out_of_range`, `not_supported`, `busy`; а також загальні, станцій/імпорту (з P25b — `level_invalid`, `level_out_of_range`), OTA (`bad_image`, `bad_magic`, `too_small`, `image_too_large`, `verify_failed`, `incomplete`, `begin_failed`, `write_failed`, `no_ota_partition`, `ota_busy`, `ota_done_restarting`, `length_required`, `unsupported_content_type`), IR). Невідомий код → «Помилка: <код>». Для `out_of_range` з `min`/`max` межі додаються до тексту автоматично.

## 8. Розробка на ПК

1. У `config/web_api_config.h` поставте `#define WEB_API_DEV_CORS 1`, перепрошийте (лише для розробки!).
2. `cd webui && python -m http.server 8000`.
3. Відкрийте `http://localhost:8000/?device=192.168.x.x` — адреса пристрою запамʼятається в `sessionStorage` вкладки (скинути: `?device=`). Усі запити підуть на `http://192.168.x.x/api/...`; зверху зʼявиться банер «Режим розробки».
4. Правки HTML/CSS/JS видно після перезавантаження сторінки, без перепрошивки. Для релізу: `WEB_API_DEV_CORS 0`, `python tools/build_web.py`, збірка, прошивка.
5. Змішаний вміст: сторінка з `http://localhost` до `http://IP` дозволена; з `https://` — ні.

## 9. Обмеження

- Усі сторінки й API — HTTP, без автентифікації.
- Кожен файл віддається з `Cache-Control: no-cache` без ETag — при кожному завантаженні сторінки браузер отримує файли повністю (gzip, ≈ 17 КБ для Головної).
- Список станцій на сторінці «Станції» опитується повільніше за Головну (`CONFIG.stationsPollMs`); якщо `station.count` у статусі не збігається з довжиною списку — список перечитується автоматично (зміна з пристрою / іншого клієнта з тією самою кількістю станцій не помітна — є кнопка «Оновити»).
- Сторінка «Пульт» під час навчання опитує статус кожні `irLearnPollMs` (400 мс) паралельно з `/api/status` (послідовно, через чергу `api`); у прихованій вкладці опитування стоїть.
- Після `reboot` / `factory-reset` сторінка чекає до `CONFIG.rebootTimeoutMs` (60 с); після скидання Wi-Fi очікування немає — адреса пристрою зміниться.
- OTA (P24): прогрес — лише передача (XHR), не запис на пристрої; автоматичного відкату й перевірки підпису / хеша немає; образ LittleFS не оновлюється (сторінки вбудовані в прошивку); під час передачі решта сторінки й опитування вимкнені, закриття вкладки перериває оновлення (пристрій відновить режим).
- Ресурси збільшують прошивку (розділ app0/app1 — 5 767 168 байт). Перед додаванням великих файлів дивіться таблицю, яку друкує `build_web.py`.
