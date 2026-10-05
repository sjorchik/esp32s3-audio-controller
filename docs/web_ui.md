# Веб-інтерфейс: каркас фронтенду (Prompt 19)

Документ для наступних промптів (P20–P23). Нові сторінки додаються **лише змінами в `webui/`** — без правок C++.
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

   Кнопка живлення й індикатор Wi-Fi в шапці — частина каркаса; їх поведінку зараз реалізує `home.js`. Для інших сторінок або винесіть цю логіку в `common.js`, або не показуйте елементи (залиште лише `#brand` і `#nav`).
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
| `icon(name) → SVGElement`, `setIcon(node, name)`, `hydrateIcons(root?)` | іконки: `power play pause stop prev next volume mute minus plus refresh search`; `hydrateIcons` обробляє `[data-icon]` |
| `rssiToBars(rssi) → 0..4`, `renderWifi(node, status.wifi)` | індикатор Wi-Fi |
| `initShell(activeId)` | заголовок, іконки, навігація, dev-банер |

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

Повний словник — об'єкт `REASONS` у `common.js` (коди керування: `standby`, `ota_in_progress`, `ir_learn_active`, `not_radio_input`, `wifi_setup`, `no_stations`, `input_unavailable`, `station_out_of_range`, `volume_out_of_range`, `gain_out_of_range`, `not_supported`, `busy`; а також загальні, станцій/імпорту, OTA, IR). Невідомий код → «Помилка: <код>». Для `out_of_range` з `min`/`max` межі додаються до тексту автоматично.

## 8. Розробка на ПК

1. У `config/web_api_config.h` поставте `#define WEB_API_DEV_CORS 1`, перепрошийте (лише для розробки!).
2. `cd webui && python -m http.server 8000`.
3. Відкрийте `http://localhost:8000/?device=192.168.x.x` — адреса пристрою запамʼятається в `sessionStorage` вкладки (скинути: `?device=`). Усі запити підуть на `http://192.168.x.x/api/...`; зверху зʼявиться банер «Режим розробки».
4. Правки HTML/CSS/JS видно після перезавантаження сторінки, без перепрошивки. Для релізу: `WEB_API_DEV_CORS 0`, `python tools/build_web.py`, збірка, прошивка.
5. Змішаний вміст: сторінка з `http://localhost` до `http://IP` дозволена; з `https://` — ні.

## 9. Обмеження

- Усі сторінки й API — HTTP, без автентифікації.
- Кожен файл віддається з `Cache-Control: no-cache` без ETag — при кожному завантаженні сторінки браузер отримує файли повністю (gzip, ≈ 17 КБ для Головної).
- Ресурси збільшують прошивку (розділ app0/app1 — 5 767 168 байт). Перед додаванням великих файлів дивіться таблицю, яку друкує `build_web.py`.
