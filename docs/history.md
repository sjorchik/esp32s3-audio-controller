# Історія проєкту ESP32-S3 Audio Controller

Журнал виконаних кроків Фази 2 (P18–P41) для планувального чату. Актуальний стан, інваріанти й індекс файлів — у `MASTER_SPEC_v61.md`; код — у репозиторії.

**Примітка про повноту:** оригінальні детальні звіти кроків P18–P29b до цього файлу не збереглись (їх не було в репозиторії), тому для них нижче — стислі резюме зі специфікації. Для P30–P41 — записи за фактичними звітами чатів із кодом. Записи в цей файл додавати лише за проханням власника.

## Стислі резюме P18–P29b

- **P18** — веб-API керування (`power`, `mute`, `volume`, `gain`, `input`, `player/*`), система (`/api/system`, `reboot`, `factory-reset`), `wifi/reset`, розширений `/api/status`; контракт — `docs/web_api.md`.
- **P19** — каркас вебу (`webui/`, `tools/build_web.py`, віддача з прошивки) + «Головна». **P20** — «Станції». **P20b** — назва станції на дисплеї й у `/api/status` = назва зі списку.
- **P21** — «Аудіо». **P21b** — профілі звуку ПО ВХОДАХ (`Settings::profiles`, NVS blob v2).
- **P22** — «Пульт» (+ виправлення таблиці IR-дій; OK і BACK навчаються). **P23** — «Налаштування» і «Система». **P23b** — назви входів із `Settings` на дисплеї. **P23c** — вихід STBY підсилювача (GPIO46).
- **P24** — OTA на сторінці «Система». **P24b** — нова таблиця розділів (`otadata` 0xE000, `nvs` 0x5000).
- **P25** — рівень виходу декодера ПО СТАНЦІЯХ (`levelDb`, цифрове послаблення). **P25b** — поле у формі станції. **P25c** — типове −20 дБ. **P26** — скасовано власником.
- **P27** — підписи «тембр НЧ» / «тембр ВЧ» у вебі, «Вхід 1…4» у «Пульті». **P28** — офлайн-режим + утримання POWER ≥ 3 с = «тихий» перезапуск. **P29** — Wi-Fi: до 5 збережених мереж, скан і `connect` у режимі STA. **P29b** — сторінка «Wi-Fi» (`/wifi`).

## P30 — 5-смуговий еквалайзер радіо (прошивка)

**Файли:** нові `config/eq_config.h`, `audio/eq.h`, `audio/eq.cpp`; змінені `audio/vu_pcm_hook.cpp`, `audio/audio_player.cpp`, `core/settings.h`/`.cpp`, `config/settings_config.h`, `net/web_api_player.h`/`.cpp`, `docs/web_api.md`.

**Аудит (було → зроблено):**
- Частота дискретизації: не віддавалась → задача плеєра після `loop()` викликає `eq::setSampleRate(s_audio->getSampleRate())` під `#if EQ_SAMPLE_RATE_FROM_LIB`; **НЕ ПЕРЕВІРЕНО** по `Audio.h` на залізі.
- NVS: blob v2 (Settings 160 Б, blob 164) → v3 (Settings 164, blob 168), поле `eqGainsDb[5]` у кінці після `profiles`; формати розрізняються довжиною + версією (144/164/168). Міграція v1→v3, v2→v3 (копіюється лише `offsetof(Settings,eqGainsDb)` байт, `eq` = 0).
- Biquad: готового не було → новий, float DF2T, peaking RBJ, коефіцієнти в double, Q = 1.0.

**Публічні інтерфейси:**
```cpp
namespace eq {
  void setBandDb(uint8_t band, int8_t db);
  int8_t getBandDb(uint8_t band);
  void setAllDb(const int8_t gains[eq_cfg::kBandCount]);
  void getAllDb(int8_t out[eq_cfg::kBandCount]);
  void setSampleRate(uint32_t hz);
  uint32_t sampleRate();
  void process(int32_t* buf, int32_t frames);
}
```
`Settings`: +`int8_t eqGainsDb[eq_cfg::kBandCount]`. `settings_cfg`: `kFormatVersion = 3`, +`kPrevFormatVersion = 2`. `registerPlayerRoutes` — сигнатура без змін, реєструє ще `GET`/`POST /api/eq`.

**Поведінка:** хук = `eq::process` → `output_trim::process` → VU. Усі смуги 0 дБ → блок не чіпається. Коефіцієнти перераховуються в хуку при зміні пресету (атомарний лічильник версії) або частоти. `SettingsStore::load()`/`resetToDefaults()` → `eq::setAllDb()`. `POST /api/eq`: `eq::setAllDb` + `SettingsStore::modify` (NVS-дебаунс, лише при зміні); 409 `ota_in_progress` під час OTA.

**Відхилення:** `/api/eq` не через `runWebCommand` (програмний DSP, не керування залізом); версію blob піднято до 3; `web_server.cpp` не змінювався.

**Ресурси:** RAM ≈ 0.4 КБ статично (коефіцієнти/стани), стек хука +~0.2 КБ; NVS blob 164→168 Б; flash ≈ кілька КБ; CPU: float, до 5 смуг × 2 канали.

**Не перевірено:** збірка на залізі (компіляція ОК, прошивка ще ні), `Audio::getSampleRate()`, навантаження CPU ядра 1 при 5 активних смугах, шум 60 Гц (float), клацання при зміні пресету наживо.

**Обмеження й ризики:** clamp при підсиленні (EQ до trim, можливий кліпінг при високих + на кількох смугах разом); смуга 12 кГц вимкнена при fs < 26.7 кГц; `Audio::getSampleRate()` може виявитись відсутнім у прикріпленій версії бібліотеки.

**Що прикріплювати до P30b (веб):** `docs/web_api.md`, `docs/web_ui.md`, `webui/js/common.js`, `webui/js/nav.js`, `webui/css/app.css`, `webui/audio.html`, `webui/js/audio.js`.

---

## P30b — еквалайзер на сторінці «Аудіо» (веб)

**Файли (змінено):** `webui/audio.html`, `webui/js/audio.js`, `webui/js/common.js`, `docs/web_ui.md`. `app.css` і C++ не чіпались.

**Публічні інтерфейси:** нових функцій `common.js` немає. `CONFIG`: `eqMinDb` −12, `eqMaxDb` 12, `eqStepDb` 1, `eqBandFreqHz` [60, 250, 1000, 4000, 12000], `eqRadioInput` 0, `eqRetryMs` 5000. `REASONS` не змінювались.

**Поведінка:** `#grp-eq` (fieldset) видимий лише при `st.input === 0`; `GET /api/eq` при першому показі й після кожної зміни входу; повзунок → `POST {band, gainDb}` через `createSlider`; «Скинути» → `POST {gainsDb:[0,0,0,0,0]}` без confirm; межі, крок і частоти беруться з відповіді API; fieldset `disabled` до першого успішного GET і при OTA / IR / WifiSetup / офлайн; помилки GET — inline, повтор через `eqRetryMs`.

**Відхилення:** немає. Секція лежить усередині `#main` (ховається без процесора).

**Перевірено власником:** працює (на пристрої).

**Не перевірено окремо:** мобільна розкладка; швидка зміна входу під час GET.

**Обмеження й ризики:** пресет не опитується (зміна з іншого клієнта видна після зміни входу / перезавантаження); без процесора секції немає.

**Що прикріплювати далі до вебу «Аудіо»:** `docs/web_ui.md`, `docs/web_api.md`, `webui/js/common.js`, `webui/js/audio.js`.

---

## P31 — автоповернення цілі регулювання енкодера на гучність (прошивка)

**Файли:** `core/app_controller.cpp`, `config/app_controller_config.h` (`.h`, `app_state.h`, `events.h`, `ui/screens.cpp` без змін).

**Було → зроблено:** `s_target` скидався лише в `begin()`; `cycleTarget()` — єдиний, хто його міняє. Додано `s_lastEncMs` (`millis()` останньої `ENC_CW`/`ENC_CCW`/`ENC_PRESS`, у `handleLocked()`); перевірка в `tickLocked()`: `s_target != Volume && reached(now, s_lastEncMs + kAdjustTimeoutMs)` → ціль = Volume + `publishState()`; скидання цілі в `enterStandby()` і `powerOnTransition()`.

**Поведінка:** будь-яка подія енкодера (обертання, клік, довге утримання/мʼют) скидає відлік; кнопки/IR/веб — ні. Через `kAdjustTimeoutMs = 5000` мс без активності ціль → Volume, лог `[APP] adjust target -> volume (timeout)`. Ціль у NVS не зберігається. Таймаут працює в усіх режимах (у меню ціль скидається, але не видна).

**Відхилення:** немає. **Ресурси:** +4 Б RAM. **Обмеження:** точність ≈ `kIdlePollMs` (50 мс).

---

## P32 — спливне вікно параметра звуку + шрифт цифр (дисплей)

**Файли:** створено `tools/gen_digits_font.py`, `ui/font_digits_data.h` (згенеровано); змінено `ui/fonts.h/.cpp`, `ui/screens.cpp`, `core/app_state.h`, `core/app_controller.cpp`, `config/screens_config.h`, `config/display_config.h`.

**Було → зроблено:** `drawStatusRow` (лінія + ціль зліва + значення справа) прибрано повністю разом із гучністю й лінією. Вікно малюється ОСТАННІМ шаром після `switch(mode)` у `frame()` (кадр перемальовується повністю, коли `dirty`; при закінченні відліку `dirty = true`).

**Інтерфейси:** `+FontSize::Digits` (в кінець enum); `AppStateData::popupTarget`, `popupSeq` (в кінець); `AppController` API без змін.

**Поведінка:** `AppController::handleLocked` викликає `popupForEvent()`: `ENC_CW/CCW` і короткий `ENC_PRESS` → поточна ціль (після `cycleTarget` — нова); `VOL/BASS/TREBLE/BALANCE/GAIN` → свій параметр. Довге `ENC_PRESS` (мʼют) і веб вікно не викликають. Відлік `kPopupTimeoutMs = 5000` мс від зміни `popupSeq` веде UI (`millis()`); поза Radio/ExternalInput і під час `restarting` вікно ховається. Підпис (Vol/Bass/Treble/Bal/Gain) — `FontSize::Large`, цифри — `FontSize::Digits`, обидва по центру.

**Відхилення:** відлік веде UI, не контролер; рамка без заокруглення (нема примітива); підпис Large замість Small (вимога власника після перевірки). **Фікс після перевірки на залізі:** LovyanGFX ставить верх цифри шрифту на верх рядка (відступ 0, не 20 px) → цифри налазили на підпис; `kPopupDigitsTopInset = 0`, відступи зменшено.

**Ресурси:** шрифт em=112, `yAdvance`=131, комірка 71, цифра 86 px, бітмап 6373 Б flash; вікно 249×126, Y=22, цифри в рядках 54..140; розкладка: рамка 2, відступ зверху 4, підпис Large 24, проміжок 2, цифри 86, відступ знизу 6.

**Обмеження:** зміна з вебу при показаному вікні оновлює число, але не продовжує таймер; перегенерація шрифту іншим TTF змінить `yAdvance` → `static_assert` у `fonts.cpp`; вертикаль підганяється `kPopupDigitsDy`.

---

## P33 — екран Radio: прибрати назву входу, обʼєднати статус і метадані

**Файли:** `ui/screens.cpp`, `config/screens_config.h`.

**Було → зроблено:** `drawRadio` малював назву входу (Small), станцію (Large marquee), метадані (Small marquee), VU і окремий статус (`drawStreamStatus`). Тепер назву входу прибрано; метадані й статус — один marquee-рядок у слоті `kTrackY` (`s_track`). `drawStreamStatus()` → `statusText(StreamStatus, uint16_t& color)` (лише вибір тексту/кольору, тексти без змін); `radioInfoLine()`: `Playing && trackTitle[0]` → трек (dim), інакше `statusText()`. Скидання marquee при зміні тексту (strcmp) і при зміні `stationIndex`.

**Поведінка:** Playing + метадані → трек; Playing без метаданих → `Playing` (зелений); Stopped (dim); Connecting/Buffering/Reconnecting (жовті); Error (червоний). ExternalInput, Standby, меню, списки, спливне вікно — без змін.

**Прибрано з `screens_config.h`:** `kTopBarTextY`, `kTopBarNameGap`, `kStateY` і 3 `static_assert`. **Обмеження:** «…» (U+2026) шрифт не малював (виправлено в P34); «метаданих немає» і «ще не прийшли» не розрізняються. Власник: перевірено.

---

## P34 — VU у стилі «Маяк-233», «…» → «...», watchdog дисплея

**Файли:** `ui/screens.cpp`, `config/screens_config.h`, `config/vu_config.h`, `audio/vu_pcm_hook.cpp`, `ui/display.cpp`.

**VU:** було 2×25 сегментів зелений/жовтий/червоний, y=88..118 → 2×41 сегмент (5 px + проміжок 2 px), x 8..293, смуга L y=94..112, шкала Tiny y=115..129, смуга R y=132..150, підписи L/R праворуч (x=299). Кольори: підсвічений бірюзовий (0,235,215), перевантаження червоний (255,50,40), непідсвічені — приглушені відтінки. Шкала `kDbFloor` = −21 дБ (було −48), лінійна за дБ, мітки −20/−14/−9/−5/−2/0; червона зона — сегменти 37..40 (`kVuRedSeg`); 2 базові сегменти світяться завжди (`kVuMinSegments`). Peak hold у UI (`VuPeak`): 100 мс утримання, 60 мс спад на сегмент. Балістика: `kAttackMs` 15 → 5, `kReleaseMs` 250 → 80.

**Вимір VU:** тепер ПІСЛЯ `eq::process`, але ДО `output_trim::process` (було після послаблення станції) — змінено інваріант P25 п.5 / P30 п.11; −20 дБ рівня станції більше не стискають шкалу.

**Watchdog:** `ui/display.cpp` (`taskLoop`): `xTaskDelayUntil` повертає `pdFALSE` при запізненні кадру → `vTaskDelay(1)` + скидання `lastWake`; усунуло `task_wdt` (IDLE0 голодувала при кадрах > 40 мс). Власник підтвердив: краш зник.

**Інше:** `statusText()`: «…» → «...». Прибрано `kVuYellowSeg`, `kVuBarGap`; `static_assert` оновлено на `kVuY+kVuH <= kH-4`.

**Відхилення:** бірюзовий замість зеленого (за фото «Маяк-233»); сегменти малюються по одному (82 `fillRect`); змінено `display.cpp` (поза межами «лише VU»); VU міряється до рівня станції. **Обмеження:** sample-peak, не RMS; стоп/обрив не гасить смуги повністю (2 сегменти завжди); при кадрах > 40 мс падає FPS (за потреби `kFramePeriodMs` → 50). Власник: перевірено.

---

## P35 — екран Radio: світліші метадані, більші шрифти

**Файли:** створено `tools/gen_xlarge_font.py`, `ui/font_xlarge_data.h` (генерується); змінено `ui/fonts.h/.cpp`, `ui/screens.cpp`, `config/display_config.h`, `config/screens_config.h`.

**Було → зроблено:** назва станції: Large 24 px, y=34 → **XLarge 36 px (em=30), y=28** (= `kTopBarY + kIconSize`, під рядком іконок), білий. Метадані/статус: Small 17 px, y=62, dim → **Large 24 px, y=66**, метадані `kColorTrack` = rgb565(210,210,210); кольори статусів без змін. Розкладка: 28+36=64; +2 → 66; 66+24=90; +4 → 94 (`kVuY`), запас `kVuGapAbove` = 4 px. Нові/змінені `static_assert`: `kTrackY + kFontLargePx + kVuGapAbove <= kVuY`; `kStationY + kFontXLargePx < kTrackY`; `kStationY >= kTopBarY + kIconSize`.

**Відхилення:** назва станції не на y=`kMargin`, а під іконками (варіант на y=8 вимагав би обмежувати ширину назви й переробляти маски marquee). **Ресурси:** flash ≈ 17.9 КБ бітмапів + ≈ 8 КБ таблиці гліфів ≈ 26 КБ; `font_data.h` не змінено. **Обмеження:** до запуску генератора збірка не проходить; метадані прокручуються частіше через ширший шрифт.

---

## P36 — екран Radio: SSID, IP і рівень сигналу Wi-Fi

**Файли:** створено `config/wifi_status_config.h`; змінено `config/screens_config.h`, `core/app_state.h`, `core/app_controller.cpp`, `ui/screens.cpp`.

**Було → зроблено:** іконка Wi-Fi залежала лише від `wifiConnected` (Wifi/WifiOff), `Wifi1..3` не використовувались; тепер `Wifi1..3` за RSSI з гістерезисом. SSID/IP уже були в `AppState` (P12), на Radio не показувались; тепер один рядок `SSID  IP` (Tiny) у смузі іконок.

**Інтерфейси:** `AppStateData` +`int8_t wifiRssi` (в кінець); `AppController`/`WifiManager` без змін.

**Поведінка:** STA підключено — один рядок `SSID  IP` (по вертикалі в рівень іконок, `kNetY`, колір `kColorNet`), IP завжди цілий, SSID обрізається `fitText` з «...»; не підключено й не офлайн — `Connecting...`; офлайн/AP — порожньо. Іконка: ≥ −60 дБм `Wifi3`, ≥ −72 `Wifi2`, інакше `Wifi1`; `WifiOff` (червона) без звʼязку; гістерезис 4 дБ (±2) у UI (`s_wifiBars`). RSSI квантується кроком 2 дБ у `syncPlayer()` (`quantizeRssi`), 0 = невідомо.

**Відхилення:** 3 рівні замість 4 (`WifiOff` зайнятий станом «нема звʼязку»), другий поріг −72; один рядок замість двох (верхній рядок на y=0 не було видно — рішення власника); квантування в окремому `config/wifi_status_config.h`.

**Обмеження:** шрифт без не-латинських/не-кириличних SSID; при довгому IP SSID скорочується сильніше. Не перевірено: нульова ініціалізація `wifiRssi` в `app_state.cpp` (файл не надавався).

---

## P37 — VU зовнішніх входів через PCM1808

**Файли:** створено `config/adc_vu_config.h`, `audio/adc_vu.{h,cpp}`, `audio/vu_math.h`; змінено `audio/vu_source.cpp` (використовує `vu_math`), `audio/audio_player.cpp` (`setPinout` + MCLK GPIO16 при `ADC_VU_ENABLE`), `main.cpp` (`initAdcVu` після `initAppController`), `ui/screens.cpp` (`VuSourceAdc`, VU в ExternalInput).

**Аудит:** такти I2S0 безперервні (`auto_clear`, `stopSong` не вимикає канал); MCLK не виводився → `setPinout` з `kI2sMclk` (256·fs); слот 32 біти стерео = 64 BCLK/кадр; I2S1 RX slave на тих самих BCLK/LRCLK (GPIO15/17) + DIN GPIO8; після init RX низькорівневий output-enable для GPIO15/17 (`gpio_ll`, НЕ `gpio_set_direction`). Стрепи PCM1808: MD0=MD1=FMT=GND (slave, I2S 24 біти, авто MCLK/fs).

**ВАЖЛИВО:** піни 17/7 TDA7318 (піни мікросхеми, не GPIO) — вихід селектора ДО регулятора гучності/тембру; VU залежить від входу й gain, не від гучності.

**Інтерфейси:** `AdcVu::begin()/isReady()`, `VuSourceAdc`; решта без змін.

**Поведінка:** захоплення лише в ExternalInput, вхід 1..3, без `restarting`; без даних → 2 базові сегменти; VU і при мʼюті. Калібровка `kAdcVuOffsetDb = 20`.

**Відхилення:** `ADC_VU_COMPENSATE_VOLUME` не реалізовано (не потрібне); `features.h`/`pins.h` не чіпали (`ADC_VU_ENABLE` замість `ENABLE_PCM1808`).

**Ресурси:** буфер 4 КБ, DMA 6×256 кадрів, задача `adc_vu` на ядрі 0, пріоритет 2, стек 4096. **Обмеження:** PT2313L не підтримується; кліпінг АЦП без подільника.

---

## P38 — екрани зовнішніх входів: назва по центру, кольори

**Файли:** `ui/screens.cpp`, `config/screens_config.h`.

Напис `INPUT` прибрано; назва входу `FontSize::XLarge` по центру області між смугою іконок і VU (`kExtAreaTop..kExtAreaBottom`, `kExtNameDy = -4`, `kExtNameY = 37`); довге імʼя — `fitText` з «...» (marquee не обрано, суперечить центруванню). Колір за входом (`kColorInputName[3]`, індекс = `inputIndex - 1`): 1 — світло-зелений (120,255,120), 2 — світло-ЧЕРВОНИЙ (255,120,120; рішення власника замість жовтого), 3 — світло-блакитний (120,190,255); поза 1..3 → `dc::kColorFg`.

**Відхилення від промпту (за проханням власника):** верхня стрічка на входах 1–3 така ж, як на Radio: `drawTopIcons(s, true)` + `drawNetInfo(s)` (іконка Wi-Fi за RSSI, мʼют, offline, рядок `SSID  IP`). Запасний `Input` у `inputName()` лишився лише на випадок порожнього `AppState` до першої публікації. Власник: перевірено.

---

## P39 — холодний старт у standby + відключення входів 1–3 (прошивка)

**Файли:** `config/app_controller_config.h`, `config/settings_config.h`, `core/settings.h/.cpp`, `core/app_state.h`, `core/app_controller.h/.cpp`, `net/web_api_player.h/.cpp`, `docs/web_api.md`.

**Аудит:** `begin()` завжди вмикав (`powerOnTransition`), гілка `kBootInStandby=false` існувала, на залізі не запускалась → холодний старт = ця гілка. Вибір входу (`stepInput`/`selectInput`/`InputSet`/`leaveStandby`/`begin`) уже йшов через `inputAvailable()` (P28) → маска додана туди. `GET /api/status.inputs[]` ({index,name,available}) не чіпався.

**Інтерфейси:** `Settings::inputEnabledMask` (після `eqGainsDb`), NVS blob v4 = 170 Б (v1/v2/v3/v4 = 144/164/168/170); `settings_cfg`: `kFormatVersion=4`, `kPrev3FormatVersion=3`, `kInputMaskAll`, `kInputMaskRadioBit`, `normalizeInputMask`; `AppStateData::inputEnabledMask`; `app_controller_cfg`: `kStandbyOnColdStart=true`, `kColdStartResetReasons[]={ESP_RST_POWERON, ESP_RST_BROWNOUT}`, `isColdStartReason()`; `WebCmdType::InputsEnabledSet`, причина `input_disabled` (→ 409). API: `GET /api/inputs`, `POST /api/inputs` (`{"enabled":[…×4]}` або `{"index":N,"enabled":bool}`; 400 `no_body/invalid_json/unknown_field/missing_field/conflicting_fields/invalid_value/out_of_range`, 409 `ota_in_progress/ir_learn_active`, 413, 503 `busy`; вхід 0 мовчки ігнорується).

**Поведінка:** холодний старт → Standby (мʼют, амп LOW, без потоку); решта скидань як раніше. Вимкнені входи пропускаються UP/DOWN, `INPUT_*` ігноруються з логом; `lastInput` вимкнений → радіо; вимкнення активного входу → `changeInput(0)`. `POST /api/inputs` дозволено в standby. Offline не стартує, якщо жоден зовнішній вхід не дозволений.

**Відхилення:** окремий `/api/inputs` замість розширення `/api/status.inputs[]`; використано наявну гілку `kBootInStandby`; 409 `input_disabled` (не 400). **Обмеження:** AP-налаштування Wi-Fi після холодного старту потребує POWER; reset-причину EN можна додати в перелік холодних. Хост-тест розкладки `Settings` і міграцій v3→v4, v2→v4 — OK. Власник: перевірено.

---

## P39b — веб: відключення входів

**Файли:** `webui/` (`settings.html` + `js/settings.js`, `index.html` + `js/home.js`, `audio.html` + `js/audio.js`, `js/common.js`), `docs/web_ui.md` (§5.8).

Секція «Входи» у налаштуваннях (картка `#inputs-list`): перемикачі входів 1–3, радіо неактивне («завжди доступне»), апаратно недоступний вхід — неактивний із поясненням; помилка → toast і повернення зі стану кешу; toast при вимкненні активного входу; не блокується OTA / IR / WifiSetup, лише offline. Вимкнені входи ПРИХОВАНО з вибору входу на «Головній» і «Аудіо».

**`common.js`:** кеш входів (`onInputsChange`, `getInputs`, `isInputEnabled`, `applyInputs`, `loadInputs`, `syncInputs(currentInput)`, `setInputEnabled`, `noteInputError`); синхронізація у циклі `refreshStatus` (перший виклик, зміна активного входу, не рідше `CONFIG.inputsRefreshMs = 10000`; `inputsRetryMs = 5000`), окремого пулера немає; `REASONS.input_disabled`. **Пропозиція на майбутнє:** додати `enabled` у `GET /api/status.inputs[]`. Власник: перевірено.

---

## P40 — README.md

Створено `README.md` у корені (українською): можливості, апаратна частина з таблицею пінів (з `pins.h`), швидкий старт (середовище PlatformIO `esp32s3`, веб вбудований у прошивку, `uploadfs` не використовується), AP-портал `AudioCtrl-Setup` / `192.168.4.1`, керування, веб-сторінки, конфігурація, архітектура, OTA, обмеження, структура репозиторію, залежності. Розділ «Ліцензія» не додано (файлу `LICENSE` немає).

**Не вдалось підтвердити (пропущено в README):** `kBootWifiResetHoldMs`, повний перелік навчальних IR-дій, призначення службових файлів у корені (`add.json`, `move.json`, `put.json`, `stations-2026-10-06.json`, `ir-map-2026-10-06.json`, `git_cheat_sheet.txt`). **Знайдені розбіжності** (виправлено в P41): `docs/web_api.md` §10/§8.4/§6, `features.h`, `pins.h`, докстрінг `gen_digits_font.py`. **Бажані зображення:** фото пристрою, скриншоти екранів Radio/ExternalInput і веб-сторінок (Головна, Аудіо з еквалайзером, Станції), схема підключення PCM5102/PCM1808/TDA7318.

---

## P41 — синхронізація документації й коментарів (поведінку НЕ змінено)

**Виправлено:** `docs/web_api.md` §6 (OK/BACK навчаються, `ENC_*` ні), §8.4 і §10 (сторінки вбудовані в прошивку, LittleFS лише для `stations.json`, `uploadfs` не використовувати); `config/features.h` — коментар `ENABLE_PCM1808` (PCM1808 підключено, реальний прапорець `ADC_VU_ENABLE`), значення `0` без змін; `config/pins.h` — коментарі `kI2sBclk/Ws/Mclk/Din` (значення й імена без змін); `tools/gen_digits_font.py` — приклад у докстрінгу `--em 112` + примітка (default `--em` лишається 80); `README.md` без змін.

**Знайдено й НЕ чіпано:** `ENABLE_PCM1808 = 0` використовується лише для логу `[MAIN] Feature PCM1808 ...` в `main.cpp` (долю прапорця вирішує власник); `platformio.ini` — хвостовий коментар радить `pio run -t buildfs / uploadfs`, що суперечить інваріанту (`uploadfs` стирає станції) — окремий крок; `features.h` — коментар `ENABLE_VU` («лише режим радіо») застарів; IR-обробники лежать у `net/web_server.cpp` (файлів `web_api_ir.*` і `core/ir_learn*` немає). Власнику лишилось підтвердити, що збірка не змінилась.
