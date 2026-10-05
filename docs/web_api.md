# HTTP API пристрою (ESP32-S3 Audio Controller)

Документ-контракт для фронтенду. Опис ПОВНИЙ і самодостатній: C++ читати не треба.
Версія контракту: Prompt 18 (прошивка `0.18.0`, див. `GET /api/system`); семантику `station.name` уточнено в Prompt 20b.

## 1. Загальні правила

- **Адреса:** `http://audio.local/` (mDNS) або IP пристрою (`wifi.ip` з `/api/status`). Порт 80. HTTPS немає.
  У режимі точки доступу (AP `AudioCtrl-Setup`, captive portal) порт 80 займає портал налаштування Wi-Fi;
  цей API там **недоступний** (див. §10).
- **Формат:** тіла запитів і відповідей — JSON (UTF-8), крім `import`, `export` та `ota`. Для POST/PUT із JSON
  обовʼязково `Content-Type: application/json`. Кожна JSON-відповідь має `Cache-Control: no-store`.
- **Автентифікації немає.** Будь-хто в мережі може керувати пристроєм.
- **Приклади ілюстративні:** числа в прикладах JSON (межі тембру/балансу, обсяги памʼяті тощо) — для ілюстрації
  формату. Реальні межі завжди беріть з `processor.capabilities` у `/api/status`.
- **Успіх:** `{"ok":true,...}` (список станцій, `/api/status`, `/api/settings`, `/api/ir/actions`,
  `/api/ir/map`, `/api/ir/learn/status`, `/api/system` — без `ok`, самі дані).
- **Помилка:** `{"ok":false,"error":"<код>"[,"field":"<поле>"][,...]}` + HTTP-код. Код `error` — стабільний
  `snake_case`, його можна мапити на текст у UI. Виняток — `POST /api/settings` (§3.2), який повертає масив `errors`.
- **Невідомий шлях або метод** → `404 {"ok":false,"error":"not_found"}`.
- **Невідоме поле в тілі** → `400 unknown_field` (друкарська помилка не вважається успіхом).
- **Ліміти тіла:** JSON-тіла ≤ 1024 байт (`413 body_too_large` інакше); `POST /api/ir/map/import` ≤ 8192 байт;
  `POST /api/stations/import` ≤ розміру файлу сховища (≈ 128 КБ); OTA — розмір вільного OTA-розділу.
- **Опитування стану:** `GET /api/status` дешевий; безпечний період — 1-2 с. Окремих push-каналів (WebSocket/SSE)
  немає. Під час навчання IR опитуйте `/api/ir/learn/status` (≈ 300-500 мс), під час OTA — `/api/status`
  (`mode:"OtaUpdate"`, `otaProgress`).
- **Одночасність:** сервер однозадачний (async_tcp); не відправляйте паралельно багато запитів, особливо запитів,
  які пишуть у flash (станції, імпорти, налаштування).

### 1.1. Коди відповідей, що повторюються

| HTTP | Коли |
|---:|---|
| 200 | успіх |
| 201 | створено (`POST /api/stations`) |
| 400 | помилка клієнта: невалідне значення, поза межами, не підтримується чипом, `no_body`, `invalid_json`, `unknown_field` |
| 404 | немає такого шляху / станції / запису |
| 409 | операція зараз недопустима (стан пристрою, зайнято, список заповнений) |
| 413 | тіло завелике |
| 500 | внутрішня помилка / збій запису |
| 502 | (лише `POST /api/settings`) команду не прийняв I2C-чип |
| 503 | зайнято (мʼютекс контролера), немає аудіопроцесора (`audio_unavailable`) |

### 1.2. Коли команди керування недоступні (HTTP 409)

Команди розділів §4 (`/api/power`, `/api/mute`, `/api/volume`, `/api/gain`, `/api/input`, `/api/player/*`)
виконує головний контролер із тими самими правилами, що кнопки й пульт. Причина — у полі `error`:

| `error` | Причина |
|---|---|
| `standby` | пристрій у standby; дозволено лише `POST /api/power` |
| `ota_in_progress` | іде запис прошивки; усе, крім опитування, заборонено |
| `ir_learn_active` | іде навчання IR-кнопки; спершу `POST /api/ir/learn/cancel` |
| `not_radio_input` | команда плеєра/станції, а активний не вхід 0 (Radio) |
| `wifi_setup` | пристрій у режимі `WifiSetup` (потоку немає) |
| `no_stations` | список станцій порожній |

### 1.3. Профілі звуку по входах (Prompt 21b)

Налаштування звуку зберігаються в NVS **окремо для кожного входу**: `volume`, `bass`, `treble`, `balance`, `gain`,
`loudness`. Усі ендпоїнти, що їх міняють (`POST /api/volume`, `/api/gain`, `/api/settings` з полями
`bass|treble|balance|loudness`), працюють з профілем **поточного** входу: значення застосовується одразу й
запамʼятовується саме для цього входу (запис у NVS — з дебаунсом, а не на кожен запит). При перемиканні входу
(`POST /api/input`, `POST /api/player/station` із `switchInput`, кнопки, пульт) і при виході зі standby пристрій
під мʼютом застосовує профіль нового входу, після чого розмʼючує звук із плавним підйомом гучності; відповідь
`GET /api/status` одразу показує значення нового входу. Профілі неактивних входів через API не читаються й не
змінюються (окремих ендпоїнтів немає). Мʼют (`mute`) глобальний і в профіль не входить. Формат запитів і
відповідей не змінився.

---

## 2. Стан: `GET /api/status`

Повний знімок для Головної сторінки. Усі поля присутні завжди (крім зазначених).

```json
{
  "mode": "Radio",
  "standby": false,
  "input": 0,
  "inputName": "WiFi Radio",
  "inputs": [
    {"index": 0, "name": "WiFi Radio", "available": true},
    {"index": 1, "name": "TV Box",     "available": true},
    {"index": 2, "name": "Computer",   "available": true},
    {"index": 3, "name": "Aux",        "available": false}
  ],
  "volume": 35, "bass": 0, "treble": 2, "balance": 0, "gain": 1,
  "mute": false, "loudness": false,
  "playing": true,
  "streamStatus": "Playing",
  "station": {"index": 4, "name": "Radio Station", "count": 23, "max": 100},
  "track": "Artist - Title",
  "wifi": {"connected": true, "apMode": false, "ssid": "Home", "ip": "192.168.1.50", "rssi": -58},
  "processor": {
    "type": "Tda7318", "ready": true, "i2cErrors": 0,
    "capabilities": {
      "bass": true, "treble": true, "balance": true, "loudness": false,
      "inputCount": 4, "volumeMin": 0, "volumeMax": 100,
      "toneMin": -14, "toneMax": 14, "fader": false, "inputGain": true,
      "balanceMin": -31, "balanceMax": 31, "gainMin": 0, "gainMax": 3
    }
  },
  "brightness": 80, "displayFlipped": false,
  "otaProgress": 0,
  "heap": 180000, "psramFree": 7000000, "uptimeMs": 123456
}
```

Примітки до полів:

- `mode`: `Standby | Radio | ExternalInput | Menu | IrLearn | WifiSetup | OtaUpdate`. `Menu` = на пристрої відкрито
  список станцій. `standby` = `mode == "Standby"` (зручніший булевий).
- `inputName` — назва поточного входу (користувацька з налаштувань, інакше типова). `inputs[]` — усі 4 логічні
  входи з назвами; **`available:false` ховайте/блокуйте** (для PT2313L вхід 3 недоступний).
- `volume` — **цільова** гучність поточного входу (під час плавного підйому після розмʼюту чип може бути тихіше).
  Шкала `capabilities.volumeMin..volumeMax` (0..100). `volume`, `bass`, `treble`, `balance`, `gain`, `loudness` —
  значення **профілю поточного входу** (§1.3); після перемикання входу вони одразу змінюються на значення нового.
- `bass/treble`: кроки по 2 дБ у межах `toneMin..toneMax`; `balance`: кроки по 1.25 дБ, «+» = правий гучніше;
  `gain` — **сирі апаратні кроки** `gainMin..gainMax` (0..3; для TDA7318 крок 6.25 дБ, для PT2313L 3.75 дБ);
  gain стосується **поточного входу** й зберігається в його профілі.
- `mute` — мʼют атенюаторів, який зробив користувач (standby/переходи в нього не входять).
- `playing` — потік активний (Playing або Buffering).
- `streamStatus`: `Idle | Connecting | Buffering | Playing | Error | Reconnecting`. Оновлюється періодично
  (≈ раз на сотні мс), тож відразу після команди може відставати на кадр.
- `station.index` — **позиційний індекс** поточної станції; `station.name` — назва станції **зі списку станцій** для `station.index` (та сама, що в `GET /api/stations` і на дисплеї; після перейменування/переміщення/видалення у списку оновлюється за ≈ сотні мс без перемикання станції). Лише якщо назви у списку немає (порожній список, індекс поза межами), підставляється ICY-назва потоку, а за її відсутності — порожній рядок;
  `station.count` — кількість станцій; `station.max` — ємність списку. `track` — ICY-заголовок треку (UTF-8);
  порожній рядок, якщо немає. Поза входом Radio `station.name`/`track` порожні.
- `wifi.rssi` — дБм (0, якщо STA не підключена). Якщо звʼязок з роутером тимчасово пропав, відповідь все ще
  приходить (якщо ви в одній мережі), але `wifi.connected` буде `false`.
- `processor.ready=false` → `capabilities:null`; аудіокоманди відповідають 503 `audio_unavailable`.
  `processor.type` — тип, з яким прошивка **зараз працює** (після зміни `processorType` в налаштуваннях тип
  зміниться лише після перезапуску).
- `loudness` — чи ввімкнена тонкомпенсація поточного входу; `capabilities.loudness=false` → ховайте перемикач.
- `otaProgress` — 0..100; осмислений лише коли `mode == "OtaUpdate"`.
- `heap`, `psramFree` — вільні байти; `uptimeMs` — мс від старту.

---

## 3. Налаштування

### 3.1. `GET /api/settings`

Збережені в NVS значення (можуть відрізнятися від поточного стану: `lastVolume` — що відновиться після
увімкнення). Поля `bass`, `treble`, `balance`, `loudness`, `lastVolume` — це збережений профіль входу
`lastInput` (див. §1.3); профілі інших входів тут не віддаються.

```json
{
  "processorType": "Tda7318",
  "inputNames": ["WiFi Radio", "TV Box", "Computer", "Aux"],
  "brightness": 80, "displayFlipped": false,
  "bass": 0, "treble": 2, "balance": 0, "loudness": false,
  "lastInput": 0, "lastStation": 4, "lastVolume": 35, "lastMute": false
}
```

### 3.2. `POST /api/settings`

Часткове оновлення: передайте лише потрібні поля (≥ 1). **Атомарно:** будь-яка помилка валідації → нічого не
застосовано.

| Поле | Тип / межі | Примітка |
|---|---|---|
| `brightness` | ціле 0..100 | застосовується одразу |
| `displayFlipped` | bool | застосовується одразу |
| `bass`, `treble` | ціле `toneMin..toneMax` | потрібен чип з підтримкою, інакше `not_supported`; стосується **поточного входу** і зберігається в його профілі |
| `balance` | ціле `balanceMin..balanceMax` | те саме |
| `loudness` | bool | `capabilities.loudness` має бути true; стосується **поточного входу**, зберігається в його профілі |
| `processorType` | `"Tda7318"` \| `"Pt2313l"` | зберігається, **діє після перезапуску** (`requiresRestart`) |
| `inputNames` | масив з **рівно 4** елементів | елемент `null` = не змінювати; рядок 1..31 байт UTF-8 без керувальних символів |

Запит:

```json
{"brightness": 60, "bass": 4, "inputNames": [null, "TV", null, null]}
```

Успіх `200`:

```json
{"ok": true,
 "results": {"brightness": {"applied": true}, "bass": {"applied": true}, "inputNames": {"applied": true}},
 "requiresRestart": []}
```

`requiresRestart` містить імена полів, що діятимуть після перезапуску (`["processorType"]`; тоді
`results.processorType = {"applied":false,"requiresRestart":true}`).

Помилка валідації `400` (або `503`, якщо єдина проблема — немає процесора):

```json
{"ok": false,
 "errors": [
   {"field": "bass", "code": "out_of_range", "min": -14, "max": 14},
   {"field": "inputNames", "code": "invalid_name", "index": 1, "maxBytes": 31}
 ]}
```

Коди `errors[].code`: `invalid_value` (не той тип / не ціле), `out_of_range` (+`min`,`max`), `not_supported`
(чип не вміє), `audio_unavailable` (→ 503), `unknown_field`, `expected_array_of_4`, `invalid_name` (+`index`,
`maxBytes`), `empty_request`. Інші помилки тіла: `400 no_body | invalid_json`, `413 body_too_large`.

Збій заліза — `502`, `ok:false`, у `results.<поле>` = `{"applied":false,"error":"i2c_failed"|"busy"}`
(`busy` — контролер зайнятий, спробуйте ще раз; `i2c_failed` — чип не відповів). Тембр/баланс/тонкомпенсація
під час OTA не застосовуються (`busy`).

> Гучність, gain, мʼют, вхід, standby — **не** тут, див. §4.

---

## 4. Керування (Prompt 18)

Усі команди — `POST`, відповідь `200`:

```json
{"ok": true,
 "state": {"mode": "Radio", "standby": false, "input": 0, "volume": 40, "mute": false, "stationIndex": 4}}
```

`state` — знімок **після** виконання команди. Поточний `streamStatus`/назву треку беріть з `GET /api/status`.
Усі команди ідемпотентні там, де це можливо (повторна «ввімкнути мʼют» нічого не ламає). Недопустимий стан →
`409` з кодом із §1.2. Контролер зайнятий → `503 busy` (повторіть).

### 4.1. `POST /api/power`

`{"state":"on"|"off"|"toggle"}` — `on` = вийти зі standby, `off` = у standby. Вихід відновлює останній
вхід/станцію з плавним підйомом гучності; вхід у standby зупиняє потік і мʼютить. Працює в усіх режимах,
крім OTA/навчання IR. Помилки: `400 invalid_value` (`field:"state"`).

### 4.2. `POST /api/mute`

`{"mute":true|false|"toggle"}` — мʼют атенюаторів (як утримання енкодера). `503 audio_unavailable`, якщо немає
чипа. Помилки: `400 invalid_value`.

### 4.3. `POST /api/volume`

Рівно одне з полів:

- `{"value":N}` — абсолютна гучність `volumeMin..volumeMax` (для повзунка);
- `{"step":N}` — відносна зміна в одиницях цієї ж шкали, `-100..100`; результат обрізається до меж (не помилка).

Помилки: `400 out_of_range` (`field`,`min`,`max`), `400 invalid_value`, `400 conflicting_fields`,
`400 missing_field`, `503 audio_unavailable`. Гучність змінюється одразу (без ramp); ramp застосовується лише
після розмʼюту/зміни входу/станції. Значення стосується **поточного входу** і зберігається в його профілі
(§1.3) з дебаунсом. У мʼюті гучність змінюється, але лишається тиша.

### 4.4. `POST /api/gain`

`{"value":N}` — підсилення **поточного** входу, сирі кроки `gainMin..gainMax` (0..3). Під час стрибка рівня
пристрій на ≈ мить мʼютить. Помилки: `400 not_supported` (`capabilities.inputGain=false`), `400 out_of_range`,
`400 invalid_value`, `503 audio_unavailable`. Gain задається окремо для кожного входу: значення зберігається в
профілі **поточного** входу (NVS, з дебаунсом) і відновлюється при поверненні на цей вхід та після перезапуску.

### 4.5. `POST /api/input`

`{"index":N}` — вибір входу `0..inputCount-1`. Індекс ≥ `inputCount` (напр. 3 для PT2313L) →
`400 out_of_range` (`min:0`,`max:inputCount-1`). Зміна входу з/на Radio зупиняє/запускає потік; мʼют і ramp — як
на пристрої. Під мʼютом застосовується профіль нового входу (гучність, бас, дискант, баланс, gain,
loudness; §1.3), тож `GET /api/status` одразу показує його значення. Якщо на пристрої відкрито список станцій, він закривається. `409 standby` у standby.

### 4.6. `POST /api/player/{play|pause|stop|toggle|next|prev}`

Без тіла. Лише на вході Radio (інакше `409 not_radio_input`), не в `WifiSetup` (`409 wifi_setup`).

| Шлях | Дія |
|---|---|
| `/api/player/play` | запустити потік поточної станції (вже грає → нічого не робить, `ok`) |
| `/api/player/pause`, `/api/player/stop` | зупинити потік і скасувати перепідключення (це одне й те саме: у радіопотоку немає позиції) |
| `/api/player/toggle` | грає/зупинено — перемкнути (як коротке OK) |
| `/api/player/next` | наступна станція (по колу; як кнопка RIGHT) |
| `/api/player/prev` | попередня станція (по колу; як кнопка LEFT) |

`409 no_stations` для `play`/`toggle`-у-play/`next`/`prev`, якщо список порожній. `404 not_found` — невідомий
підшлях. «Грає чи ні» дивіться в `playing`/`streamStatus` (`Idle` = зупинено).

### 4.7. `POST /api/player/station`

`{"index":N}` — зіграти станцію за **позиційним** індексом (`0..station.count-1`).
Додатково `"switchInput":true`: якщо зараз активний не вхід Radio — перейти на Radio і зіграти станцію (без
прапорця → `409 not_radio_input`). Та сама станція вже грає → нічого не робить; зупинена → запускає.
Помилки: `400 index_out_of_range` (`field:"index"`), `400 invalid_value`, `409 no_stations`, `409 wifi_setup`.

---

## 5. Станції (`/api/stations*`)

Станція = `{"name": "...", "url": "http(s)://..."}`. Обмеження (за розміром буферів `Station`): `name`
1..63 байт UTF-8 без керувальних символів і не лише з пробілів; `url` ≤ 191 байт, починається з `http://` або
`https://` (m3u/pls-плейлисти розбирає плеєр). Максимум станцій — `station.max` у `/api/status`.

> **Індекс станції ПОЗИЦІЙНИЙ.** Стабільного `id` немає. Додавання зберігає індекси попередніх; видалення,
> переміщення й імпорт **зсувають** індекси. Після будь-якої зміни списку перечитайте `GET /api/stations`.
> Поточна станція в `/api/status` (`station.index`) теж позиційна: після видалення/переміщення станцій перед
> нею індекс може вказувати вже на іншу станцію (потік, що грає, при цьому не переривається).

### 5.1. `GET /api/stations`

```json
[{"index": 0, "name": "Radio One", "url": "http://example.com/stream"}, {"index": 1, "name": "...", "url": "..."}]
```

### 5.2. `POST /api/stations` — додати в кінець

Тіло `{"name":"...","url":"..."}` (обидва поля обовʼязкові, інших нема). `201 {"ok":true,"index":N}`.
Помилки `400` з `field`: `unknown_field`, `name_required`, `name_too_long`, `name_invalid`, `url_required`,
`url_too_long`, `url_invalid`, `url_invalid_scheme`; `409 list_full`; `500 storage_error`.

### 5.3. `PUT /api/stations/{index}` — повна заміна

Тіло як у 5.2. `200 {"ok":true,"index":N}`; `400 invalid_index` (не число); `404 not_found`; помилки валідації як у 5.2.

### 5.4. `DELETE /api/stations/{index}`

`200 {"ok":true}`; `400 invalid_index`; `404 not_found`; `500 storage_error`.

### 5.5. `POST /api/stations/move`

`{"from":N,"to":M}` — станція стає на позицію `to`, решта зсувається. `200 {"ok":true}`; `400 invalid_value |
index_out_of_range | unknown_field` (з `field`).

### 5.6. `POST /api/stations/import?format=m3u|pls|json`

Тіло — **сирий вміст файлу** (не multipart), ≤ розміру файлу сховища (≈ 128 КБ). Формат: `?format=` (головне) або `Content-Type`
(`application/json`, `audio/x-mpegurl`, `audio/x-scpls`); інакше `400 unknown_format`.
**Повністю замінює** список; при помилці старий список лишається.

Успіх: `200 {"ok":true,"format":"m3u","count":23,"replaced":true}`.
Помилка: `{"ok":false,"error":"import_failed","reason":"<код>","format":"m3u"}` з `400` (вміст), `500`
(`out_of_memory`, `io_error`, `storage_write_failed`, `file_not_found`) або `503` (`busy`).
Значення `reason`: `busy`, `out_of_memory`, `file_not_found`, `empty`, `too_big`, `version_mismatch`,
`parse_error`, `too_many_stations`, `io_error`, `storage_write_failed`.
Інші: `400 no_body | unknown_format`, `409 import_busy` (одночасний імпорт), `413 body_too_large`, `500 upload_failed`.

### 5.7. `GET /api/stations/export`

Файл `stations.json` (`Content-Disposition: attachment`): `{"version":1,"stations":[{"name":"..","url":".."}]}`.
Той самий формат приймає `import?format=json`.

---

## 6. Пульт IR (`/api/ir*`)

Імена дій — рядки на кшталт `VOL_UP`, `BASS_UP`; авторитетний перелік навчальних дій — `GET /api/ir/actions`
(OK, BACK та дій енкодера в ньому немає: таких кнопок на пульті не існує).

### 6.1. `GET /api/ir/actions`

Усі дії, які можна вивчити, зі станом:

```json
[{"action": "VOL_UP", "learned": true, "addr": 0, "cmd": 16}, {"action": "MUTE", "learned": false}]
```

`addr`/`cmd` лише коли `learned:true`.

### 6.2. `GET /api/ir/map`

Мапа кодів у форматі імпорту/експорту:

```json
[{"action": "VOL_UP", "addr": 0, "cmd": 16, "rc5x": false}]
```

Експорт = збереження відповіді цього запиту у файл.

### 6.3. `POST /api/ir/map/import`

Тіло — масив у форматі 6.2 (≤ 8192 байт). **Замінює** мапу цілком, атомарно. `200 {"ok":true,"count":N}`;
`400 import_failed` (+`mapSize`) | `invalid_json` | `no_body`; `409 learning_active`; `413 body_too_large`.

### 6.4. `DELETE /api/ir/map/{ACTION}`

Очистити код однієї кнопки. `200 {"ok":true,"count":N}`; `400 invalid_action | unknown_action`;
`404 no_code_for_action`; `409 learning_active`.

### 6.5. Навчання

1. `POST /api/ir/learn` `{"action":"BASS_UP"}` → `200 {"ok":true,"status":"Waiting","target":"BASS_UP","conflictWith":null}`.
   Помилки: `400 unknown_action | not_learnable | invalid_value`, `409 already_learning | cannot_start`
   (мапа заповнена, іде OTA, контролер зайнятий).
2. Опитуйте `GET /api/ir/learn/status` → `{"status","target","conflictWith"}`.
   `status`: `Idle | Waiting | Confirm | Success | Timeout | Conflict`. Користувач двічі натискає кнопку пульта
   (`Waiting` → `Confirm` → `Success`). `Success`/`Timeout` **залишаються** у статусі, поки не почнеться нове навчання
   або не буде `cancel` — не пропустите результат. `conflictWith` — дія, до якої вже привʼязаний цей код
   (лише в `Conflict`); `target` = `null` в `Idle`.
3. `Conflict`: `POST /api/ir/learn/confirm-overwrite` (перепризначити; `409 not_in_conflict` інакше) або
   `POST /api/ir/learn/cancel`.
4. `POST /api/ir/learn/cancel` — скасувати (ідемпотентно; поза навчанням скидає останній результат у `Idle`).
   `503 busy`, якщо контролер зайнятий — повторіть.

Під час навчання пристрій лишається в режимі `IrLearn`, а команди керування (§4) відповідають
`409 ir_learn_active`.

---

## 7. Wi-Fi

У режимі STA змінити мережу з вебу **не можна** (немає API сканування/підключення): облікові дані вводяться у
порталі точки доступу. Робочий сценарій «змінити мережу»: `POST /api/wifi/reset` → пристрій перезапускається
і піднімає AP `AudioCtrl-Setup` (адреса порталу 192.168.4.1) → користувач обирає мережу там.

Поточний стан Wi-Fi читається в `/api/status` (`wifi`) і `/api/system` (`wifi`).

### `POST /api/wifi/reset`

Тіло `{"confirm":true}` (обовʼязково; без нього `400 confirm_required`). Стирає збережену мережу й
перезапускає пристрій; після старту він у режимі AP. Налаштування, станції й мапа IR не чіпаються.

```json
{"ok": true, "restarting": true, "inMs": 1500, "next": "ap_setup"}
```

Після цієї відповіді пристрій зникне з мережі приблизно через `inMs` мс. `409 ota_in_progress | restart_pending`.

---

## 8. Система

### 8.1. `GET /api/system`

```json
{
  "firmware": {"version": "0.18.0", "buildDate": "Oct  4 2026", "buildTime": "17:42:11", "sdk": "v5.5.2"},
  "chip": {"model": "ESP32-S3", "revision": 0, "cores": 2, "cpuMhz": 240},
  "mode": "Radio",
  "uptimeMs": 123456,
  "resetReason": "Software", "resetReasonCode": 3,
  "heap": {"total": 330000, "free": 180000, "min": 150000, "largestBlock": 110000},
  "psram": {"total": 8386308, "free": 7000000},
  "flash": {"sizeBytes": 16777216},
  "fs": {"totalBytes": 5046272, "usedBytes": 200000},
  "ota": {"runningPartition": "app0", "nextPartition": "app1", "maxImageBytes": 5767168},
  "wifi": {"state": "Connected", "mode": "STA", "ssid": "Home", "ip": "192.168.1.50", "rssi": -58},
  "settings": {"dirty": false, "writeCount": 12}
}
```

- `firmware.version` — константа в `config/web_api_config.h`; `buildDate/buildTime` — час компіляції файлу
  `net/web_api_system.cpp` (не обовʼязково останньої збірки всього проєкту).
- `resetReason`: `PowerOn | External | Software | Panic | IntWatchdog | TaskWatchdog | OtherWatchdog | DeepSleep |
  Brownout | Sdio | Other` (`resetReasonCode` — сирий код ESP-IDF).
- `ota.maxImageBytes` — верхня межа розміру образу для `POST /api/ota`.
- `wifi.state`: `Connecting | Connected | ApMode | ApClientConnected`.
- Запит викликає обхід файлової системи для `fs.usedBytes` — не опитуйте його часто (≥ 5 с).

### 8.2. `POST /api/system/reboot`

Без тіла. Відкладений перезапуск (відповідь надсилається до перезапуску; відкладені налаштування
скидаються у NVS):

```json
{"ok": true, "restarting": true, "inMs": 1500}
```

Далі опитуйте `/api/status`, поки пристрій не повернеться (≈ 5-15 с). `409 ota_in_progress | restart_pending`.

### 8.3. `POST /api/system/factory-reset`

Тіло `{"confirm":true}` (обовʼязково). Відновлює **налаштування** (тип процесора, назви входів, яскравість,
орієнтація, **профілі звуку всіх входів** (гучність, бас, дискант, баланс, gain, loudness), останній
вхід/станція) до заводських і перезапускає пристрій.
**Не чіпає:** список станцій, мапу IR, збережену мережу Wi-Fi (для неї `POST /api/wifi/reset`). Відповідь
як у 8.2.

### 8.4. `POST /api/ota`

Оновлення прошивки: тіло — **сирий** `.bin` (`Content-Type: application/octet-stream`, **не** multipart і не
`application/x-www-form-urlencoded`), обовʼязково з `Content-Length` (chunked не підтримується).

```
curl -X POST --data-binary @firmware.bin -H "Content-Type: application/octet-stream" http://audio.local/api/ota
```

У браузері: `fetch('/api/ota', {method:'POST', headers:{'Content-Type':'application/octet-stream'}, body: file})`
(для прогресу — `XMLHttpRequest.upload.onprogress`; прогрес **запису** на пристрої дивіться в
`/api/status`: `mode:"OtaUpdate"`, `otaProgress`).

Успіх `200`: `{"ok":true,"restarting":true}`; пристрій перезапускається приблизно через 1.5 с і запускає нову
прошивку. Помилка: `{"ok":false,"error":"<код>","reason":"<текст>"}`:

| HTTP | `error` | Значення |
|---:|---|---|
| 400 | `bad_image` (`reason`: `bad_magic` \| `too_small`) | це не образ прошивки ESP32 (перший байт ≠ 0xE9 або < 4096 байт) |
| 400 | `verify_failed` / `incomplete` | образ не пройшов перевірку / тіло обірвалось |
| 400 | `no_body` | порожнє тіло |
| 409 | `ota_busy` / `ota_done_restarting` | інше OTA вже триває / вже записано, чекайте перезапуску |
| 411 | `length_required` | немає `Content-Length` |
| 413 | `image_too_large` | не влізає в OTA-розділ (`ota.maxImageBytes`) |
| 415 | `unsupported_content_type` | форма або multipart замість сирого тіла |
| 500 | `begin_failed` / `write_failed` / `no_ota_partition` | помилка запису |
| 503 | `busy` | іде навчання IR або контролер зайнятий |

На час OTA звук зупиняється, пристрій мʼютиться, екран показує прогрес; на помилці звук і режим
відновлюються. Оновлюється **лише прошивка**: образ LittleFS (сторінки `data/`) цим API не оновлюється — для
нього `pio run -t uploadfs` по USB (операція **стирає весь розділ LittleFS разом зі списком станцій**).
Автоматичного відкату на попередню прошивку, якщо нова не стартує, немає.

---

## 9. Підсумкова таблиця маршрутів

| Метод | Шлях | Розділ |
|---|---|---|
| GET | `/api/status` | §2 |
| GET / POST | `/api/settings` | §3 |
| POST | `/api/power`, `/api/mute`, `/api/volume`, `/api/gain`, `/api/input` | §4 |
| POST | `/api/player/{play,pause,stop,toggle,next,prev}`, `/api/player/station` | §4 |
| GET / POST | `/api/stations`; PUT / DELETE `/api/stations/{index}`; POST `/api/stations/move`, `/api/stations/import`; GET `/api/stations/export` | §5 |
| GET | `/api/ir/actions`, `/api/ir/map`, `/api/ir/learn/status` | §6 |
| POST | `/api/ir/map/import`, `/api/ir/learn`, `/api/ir/learn/confirm-overwrite`, `/api/ir/learn/cancel` | §6 |
| DELETE | `/api/ir/map/{ACTION}` | §6 |
| POST | `/api/wifi/reset` | §7 |
| GET | `/api/system` | §8 |
| POST | `/api/system/reboot`, `/api/system/factory-reset`, `/api/ota` | §8 |

---

## 10. Обмеження, про які треба знати фронтенду

- **Статичні сторінки** з LittleFS наразі не віддаються (окреме завдання); API відповідає лише на `/api/*`.
- У режимі AP (портал Wi-Fi) цей API недоступний.
- Автентифікації й HTTPS немає; пароль/токен не передавайте.
- Індекси станцій позиційні (§5). Звукові значення — по входах (§1.3); через API доступний лише профіль поточного
  входу. Назву/URL станції редагуйте лише повною заміною (PUT).
- Для розробки сторінок на ПК у `config/web_api_config.h` є прапорець `WEB_API_DEV_CORS` (за замовчуванням 0): при 1
  кожна відповідь отримує CORS-заголовки, а preflight `OPTIONS /api/*` — `204`. У релізі тримайте 0.
