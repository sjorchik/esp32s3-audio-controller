#!/usr/bin/env python3
"""Smoke-перевірка HTTP API пристрою (ESP32-S3 Audio Controller). Лише stdlib Python 3.

Запуск:
    python tools/web_api_smoke.py audio.local             # тільки безпечні GET
    python tools/web_api_smoke.py audio.local --write     # + зміна й ПОВЕРНЕННЯ значень
    python tools/web_api_smoke.py 192.168.1.50 --write --player

Режими:
    (без прапорців)   лише GET. Нічого не змінює на пристрої.
    --write           POST/PUT/DELETE-перевірки: гучність, мʼют, баланс і чутливість VU (активний і ВСІ неактивні входи, у тому
                      числі слот 3), яскравість, вхід (без зміни), слот 3 (вимкнення / увімкнення, ім'я, EQ «Player», масиви
                      з 3 і 4 елементів), станція-тимчасова (додається й видаляється), негативні випадки
                      (очікувані 400/404: застарілі ключі /api/settings, /api/gain тощо).
                      Кожна зміна повертається до початкового значення (маска слота 3, його ім'я, профіль EQ / баланс / VU
                      і гучність «Player» включно).
    --player          (разом з --write) перемикання play/pause та next/prev станції ТА вхід у режим «USB» (POST /api/input 3) з
                      виходом назад: усе чутно, потрібна флешка (без неї режим теж піднімається, але частину перевірок буде
                      пропущено).
    --confirm-checks  (разом з --write) негативні перевірки factory-reset / wifi-reset БЕЗ підтвердження
                      (очікується 400 confirm_required). Якщо прошивка має баг і не перевіряє confirm,
                      пристрій скине налаштування/Wi-Fi й перезапуститься — тому окремий прапорець.

НІКОЛИ не запускається: POST /api/system/reboot, /api/ota, /api/stations/import, /api/ir/map/import,
/api/power (standby), реальні factory-reset / wifi-reset.

Оновлено в P50a: без processor / gain / тембру; додано GET /api/balance, menuContext (на той час 3 входи; тепер 4, див. INP4b).
Оновлено у V2: GET /api/vu, status.vuSens; з --write — чутливість VU активного й неактивного входу (зміна й повернення),
негативні випадки POST /api/vu (value поза діапазоном, невідомий ключ, input = 4).

Оновлено в USB15: GET /api/usb/status|list|result (форма відповідей; за режиму «USB» вимкненому — 409 usb_mode_off на list); з --write — негативні
перевірки POST /api/usb/* (400 за тілом; 409 usb_mode_off, лише коли режим вимкнений). Нічого не змінює: POST /api/usb/mode з коректною дією
(вхід у режим) НЕ викликається — це перевіряється вручну за hwcheck_USB15.md. Якщо /api/usb/status повертає 404 (збірка з
FEATURE_USB_MODE = 0), усі USB-перевірки позначаються SKIP.

Оновлено в INP4b: 4 входи (3 логічні + слот 3 «Player» / USB). Перевіряються: GET /api/inputs, GET /api/eq?input=N, status.input == 3
у режимі «USB», POST /api/input 3 -> 202 usb_enter (повторно 200), 409 input_active (вимкнення слота 3 у режимі), index 4 ->
out_of_range (max 3), масиви з 3 елементів (застаріле) НЕ змінюють слот 3 (маска й ім'я), EQ / баланс / VU слота 3 get / set.
Режим «USB» піднімається лише з --write --player (і лише з увімкненого пристрою на вході Radio / ExternalInput).

Оновлено в USB18a: nowPlaying.path/dir/title/artist/album і queue.pos (типи; з --write --player — реальне відтворення: path відповідає
запущеному файлу, теги після next, seek за кінець -> atEnd, наступний трек без read_error, повернення до початкового треку).

Код виходу: 0 — немає FAIL; 1 — є хоча б один FAIL; 2 — пристрій недоступний.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"
VOLUME_MIN, VOLUME_MAX = 0, 100          # audio_cfg::kVolumeUiMin / kVolumeUiMax (processor.capabilities прибрано в P50a)
LOGICAL_COUNT = 3                        # defaults::kInputCount (0 радіо, 1 TV Box, 2 Computer)
SLOT_COUNT = 4                           # [INP4b] defaults::kInputSlotCount: слоти входів веб-API (0..2 логічні + слот 3)
USB_SLOT = 3                             # [INP4b] defaults::kUsbInputIndex: слот 3 = «Player» / режим «USB»
BALANCE_MIN, BALANCE_MAX = -20, 20       # balance_cfg::kMinDb / kMaxDb
VU_MIN, VU_MAX, VU_STEP = -20, 20, 1     # vu_sens_cfg::kMinDb / kMaxDb / kStepDb (V2)
MENU_CONTEXTS = ("None", "StationList", "EqBalance")
REMOVED_STATUS_KEYS = ("processor", "bass", "treble", "gain", "loudness")
REMOVED_SETTINGS_KEYS = ("processorType", "bass", "treble", "balance", "loudness")
RETRIES = 3          # спроб на запит при мережевій помилці (не при HTTP-кодах)
RETRY_PAUSE_S = 0.7  # пауза між спробами
results = []


class Dev:
    def __init__(self, host, timeout):
        self.base = host if host.startswith("http") else "http://" + host
        self.base = self.base.rstrip("/")
        self.timeout = timeout

    def call(self, method, path, body=None):
        """-> (status, parsed_json_or_text_or_None). Мережева помилка -> (0, str)."""
        data = None
        headers = {}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
        status, raw = 0, b""
        for attempt in range(RETRIES):
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as r:
                    status, raw = r.status, r.read()
                break
            except urllib.error.HTTPError as e:
                status, raw = e.code, e.read()
                break
            except Exception as e:  # noqa: BLE001 - мережева помилка (часто збій mDNS у Windows)
                if attempt == RETRIES - 1:
                    return 0, str(e)
                time.sleep(RETRY_PAUSE_S)
        text = raw.decode("utf-8", "replace")
        try:
            return status, json.loads(text) if text else None
        except ValueError:
            return status, text

    def get(self, path):
        return self.call("GET", path)

    def post(self, path, body=None):
        return self.call("POST", path, body if body is not None else None)


def record(name, ok, detail="", skip=False):
    results.append((SKIP if skip else (PASS if ok else FAIL), name, detail))


def has_keys(obj, keys):
    return isinstance(obj, dict) and all(k in obj for k in keys)


def expect(name, got, want_status, want_keys=None, want_error=None):
    status, body = got
    ok = status == want_status
    detail = "HTTP %s" % status
    if ok and want_keys:
        miss = [k for k in want_keys if not (isinstance(body, dict) and k in body)]
        if miss:
            ok, detail = False, "немає полів: %s" % ", ".join(miss)
    if ok and want_error is not None:
        err = body.get("error") if isinstance(body, dict) else None
        if err != want_error:
            ok, detail = False, "error=%r, очікували %r" % (err, want_error)
    if not ok and status != want_status:
        detail = "HTTP %s (очікували %s) %s" % (status, want_status, str(body)[:100])
    record(name, ok, detail)
    return ok, body


# --------------------------------------------------------------------------
# Безпечні GET-перевірки
# --------------------------------------------------------------------------
def read_checks(d):
    ok, st = expect("GET /api/status", d.get("/api/status"), 200,
                    ["mode", "menuContext", "standby", "input", "inputName", "inputs", "volume", "balance", "vuSens", "mute",
                     "playing", "streamStatus", "station", "track", "wifi", "otaProgress", "heap", "uptimeMs"])
    if ok:
        record("status.station.{index,count,max}", has_keys(st["station"], ["index", "count", "max", "name"]))
        record("status.input ∈ 0..%d" % (SLOT_COUNT - 1), isinstance(st["input"], int) and 0 <= st["input"] < SLOT_COUNT,
               "got %r" % (st["input"],))
        record("status.inputs[] має index/name/available, рівно %d" % SLOT_COUNT,
               isinstance(st["inputs"], list) and len(st["inputs"]) == SLOT_COUNT and
               all(has_keys(i, ["index", "name", "available"]) for i in st["inputs"]))
        gone = [k for k in REMOVED_STATUS_KEYS if k in st]
        record("status без застарілих полів (processor/bass/treble/gain/loudness)", not gone,
               "ще є: %s" % ", ".join(gone) if gone else "")
        record("status.menuContext ∈ {None, StationList, EqBalance}", st["menuContext"] in MENU_CONTEXTS,
               "got %r" % (st["menuContext"],))
        record("status.menuContext == None поза mode Menu", st["mode"] == "Menu" or st["menuContext"] == "None",
               "mode=%s menuContext=%s" % (st["mode"], st["menuContext"]))
        record("status.balance ціле в %d..%d" % (BALANCE_MIN, BALANCE_MAX),
               isinstance(st["balance"], int) and BALANCE_MIN <= st["balance"] <= BALANCE_MAX,
               "got %r" % (st["balance"],))
        record("status.vuSens ціле в %d..%d" % (VU_MIN, VU_MAX),
               isinstance(st["vuSens"], int) and not isinstance(st["vuSens"], bool) and VU_MIN <= st["vuSens"] <= VU_MAX,
               "got %r" % (st["vuSens"],))
    ok_s, sett = expect("GET /api/settings", d.get("/api/settings"), 200,
                        ["inputNames", "brightness", "displayFlipped", "lastInput", "lastStation", "lastVolume",
                         "lastMute"])
    if ok_s:
        gone = [k for k in REMOVED_SETTINGS_KEYS if k in sett]
        record("settings без застарілих полів (processorType/bass/treble/balance/loudness)", not gone,
               "ще є: %s" % ", ".join(gone) if gone else "")
        record("settings.inputNames рівно %d" % SLOT_COUNT,
               isinstance(sett["inputNames"], list) and len(sett["inputNames"]) == SLOT_COUNT)

    # --- [INP4b] GET /api/inputs (4 слоти) ---
    inp = get_inputs(d)
    expect("GET /api/inputs", d.get("/api/inputs"), 200, ["inputs", "enabledMask", "input"])
    if inp is None:
        record("inputs[] рівно %d" % SLOT_COUNT, False, "відповідь не має рівно %d входів" % SLOT_COUNT)
    else:
        arr = inp["inputs"]
        fields = ["index", "name", "available", "enabled", "selectable", "canDisable"]
        record("inputs[] рівно %d, index 0..%d, поля %s" % (SLOT_COUNT, SLOT_COUNT - 1, "/".join(fields)),
               all(has_keys(i, fields) for i in arr) and [i["index"] for i in arr] == list(range(SLOT_COUNT)), str(arr)[:100])
        if all(has_keys(i, fields) for i in arr):
            bits = sum((1 << i["index"]) for i in arr if i["enabled"])
            record("inputs.enabledMask 4-бітний (0..15), біт 0 = 1, збігається з enabled[]",
                   isinstance(inp["enabledMask"], int) and 0 <= inp["enabledMask"] <= 15 and inp["enabledMask"] & 1 and
                   inp["enabledMask"] == bits, "mask=%s, з enabled[]=%s" % (inp["enabledMask"], bits))
            record("inputs: selectable == available && enabled; canDisable == (index != 0)",
                   all(i["selectable"] == (i["available"] and i["enabled"]) and i["canDisable"] == (i["index"] != 0) for i in arr))
            record("inputs[0] (радіо): available, enabled, !canDisable",
                   arr[0]["available"] and arr[0]["enabled"] and not arr[0]["canDisable"])
            record("inputs[1..2] (логічні входи): available == true", all(arr[i]["available"] for i in (1, 2)))
        if ok:
            record("inputs.input == status.input", inp["input"] == st["input"], "%s vs %s" % (inp["input"], st["input"]))
            record("inputs[].name == status.inputs[].name",
                   all(inp["inputs"][i]["name"] == st["inputs"][i]["name"] for i in range(SLOT_COUNT)
                       if isinstance(st["inputs"], list) and len(st["inputs"]) == SLOT_COUNT))
        if ok_s:
            record("inputs[].name == settings.inputNames (непорожні)",
                   all(sett["inputNames"][i] in ("", inp["inputs"][i]["name"]) for i in range(SLOT_COUNT)
                       if isinstance(sett["inputNames"], list) and len(sett["inputNames"]) == SLOT_COUNT))

    # --- [INP4b] GET /api/eq (усі 4 слоти) ---
    ok_e, eqa = expect("GET /api/eq (активний вхід)", d.get("/api/eq"), 200,
                       ["input", "gainsDb", "freqHz", "minDb", "maxDb", "stepDb"])
    if ok_e and ok:
        record("eq: input == status.input", eqa["input"] == st["input"], "%s vs %s" % (eqa["input"], st["input"]))
    for n in range(SLOT_COUNT):
        okn, en = expect("GET /api/eq?input=%d" % n, d.get("/api/eq?input=%d" % n), 200,
                         ["input", "gainsDb", "freqHz", "minDb", "maxDb", "stepDb"])
        if okn:
            record("eq?input=%d: input == %d, 5 цілих у межах minDb..maxDb, freqHz[5]" % (n, n),
                   en["input"] == n and isinstance(en["gainsDb"], list) and len(en["gainsDb"]) == 5 and
                   isinstance(en["freqHz"], list) and len(en["freqHz"]) == 5 and
                   all(isinstance(g, int) and en["minDb"] <= g <= en["maxDb"] for g in en["gainsDb"]), str(en)[:100])
    status, body = d.get("/api/eq?input=%d" % SLOT_COUNT)
    record("GET /api/eq?input=%d -> 400 out_of_range (max %d)" % (SLOT_COUNT, SLOT_COUNT - 1),
           status == 400 and isinstance(body, dict) and body.get("error") == "out_of_range" and
           body.get("field") == "input" and body.get("min") == 0 and body.get("max") == SLOT_COUNT - 1,
           "HTTP %s %s" % (status, str(body)[:80]))
    expect("GET /api/eq?input=x -> 400", d.get("/api/eq?input=x"), 400, ["error"], "invalid_value")

    # --- GET /api/balance (P50a) ---
    ok_b, bal = expect("GET /api/balance (активний вхід)", d.get("/api/balance"), 200, ["input", "balance"])
    if ok_b and ok:
        record("balance: input == status.input", bal["input"] == st["input"], "%s vs %s" % (bal["input"], st["input"]))
        record("balance: balance == status.balance", bal["balance"] == st["balance"],
               "%s vs %s" % (bal["balance"], st["balance"]))
    for n in range(SLOT_COUNT):
        okn, bn = expect("GET /api/balance?input=%d" % n, d.get("/api/balance?input=%d" % n), 200, ["input", "balance"])
        if okn:
            record("balance?input=%d: input == %d, значення в межах" % (n, n),
                   bn["input"] == n and isinstance(bn["balance"], int) and BALANCE_MIN <= bn["balance"] <= BALANCE_MAX,
                   str(bn))
    status, body = d.get("/api/balance?input=%d" % SLOT_COUNT)
    record("GET /api/balance?input=%d -> 400 out_of_range" % SLOT_COUNT,
           status == 400 and isinstance(body, dict) and body.get("error") == "out_of_range" and
           body.get("field") == "input" and body.get("min") == 0 and body.get("max") == SLOT_COUNT - 1,
           "HTTP %s %s" % (status, str(body)[:80]))
    expect("GET /api/balance?input=-1 -> 400", d.get("/api/balance?input=-1"), 400, ["error"], "out_of_range")
    expect("GET /api/balance?input=x -> 400", d.get("/api/balance?input=x"), 400, ["error"], "invalid_value")

    # --- GET /api/vu (V2) ---
    ok_v, vu = expect("GET /api/vu", d.get("/api/vu"), 200, ["input", "minDb", "maxDb", "stepDb", "inputs"])
    if ok_v:
        record("vu: межі й крок == %d / %d / %d" % (VU_MIN, VU_MAX, VU_STEP),
               vu["minDb"] == VU_MIN and vu["maxDb"] == VU_MAX and vu["stepDb"] == VU_STEP,
               "%s / %s / %s" % (vu["minDb"], vu["maxDb"], vu["stepDb"]))
        vins = vu["inputs"]
        good = (isinstance(vins, list) and len(vins) == SLOT_COUNT and
                all(has_keys(i, ["index", "sensitivityDb"]) for i in vins) and
                [i["index"] for i in vins] == list(range(SLOT_COUNT)))
        record("vu.inputs[] рівно %d, index 0..%d, має sensitivityDb" % (SLOT_COUNT, SLOT_COUNT - 1), good, str(vins)[:100])
        if good:
            record("vu: sensitivityDb цілі в %d..%d" % (VU_MIN, VU_MAX),
                   all(isinstance(i["sensitivityDb"], int) and not isinstance(i["sensitivityDb"], bool) and
                       VU_MIN <= i["sensitivityDb"] <= VU_MAX for i in vins), str(vins)[:100])
        if ok:
            record("vu: input == status.input", vu["input"] == st["input"], "%s vs %s" % (vu["input"], st["input"]))
            if good and not st["standby"] and 0 <= st["input"] < SLOT_COUNT:
                # у standby AppState.vuSensDb не гарантовано відображає профіль -> порівнюємо лише при працюючому пристрої
                record("vu: inputs[active].sensitivityDb == status.vuSens",
                       vins[st["input"]]["sensitivityDb"] == st["vuSens"],
                       "%s vs %s" % (vins[st["input"]]["sensitivityDb"], st["vuSens"]))

    # --- видалені маршрути ---
    expect("GET /api/gain -> 404", d.get("/api/gain"), 404, ["error"], "not_found")

    status, lst = d.get("/api/stations")
    good = status == 200 and isinstance(lst, list) and all(has_keys(s, ["index", "name", "url"]) for s in lst)
    record("GET /api/stations", good, "HTTP %s, %s станцій" % (status, len(lst) if isinstance(lst, list) else "?"))
    if ok and good:
        record("status.station.count == len(/api/stations)", st["station"]["count"] == len(lst),
               "%s vs %s" % (st["station"]["count"], len(lst)))

    status, exp = d.get("/api/stations/export")
    record("GET /api/stations/export", status == 200 and has_keys(exp, ["version", "stations"]),
           "HTTP %s" % status)

    status, acts = d.get("/api/ir/actions")
    record("GET /api/ir/actions", status == 200 and isinstance(acts, list) and
           all(has_keys(a, ["action", "learned"]) for a in acts), "HTTP %s, %s дій" %
           (status, len(acts) if isinstance(acts, list) else "?"))
    if status == 200 and isinstance(acts, list):
        names = [a["action"] for a in acts]
        # З P22 OK і BACK навчаються; дії енкодера ENC_* та зарезервовані BASS_/TREBLE_/GAIN_/INPUT_AUX - ні.
        record("ir/actions містить OK і BACK", "OK" in names and "BACK" in names)
        bad = [n for n in names if n.startswith(("ENC_", "BASS_", "TREBLE_", "GAIN_")) or n == "INPUT_AUX"]
        record("ir/actions без ENC_* / BASS_* / TREBLE_* / GAIN_* / INPUT_AUX", not bad, ", ".join(bad))

    status, mp = d.get("/api/ir/map")
    record("GET /api/ir/map", status == 200 and isinstance(mp, list), "HTTP %s" % status)
    expect("GET /api/ir/learn/status", d.get("/api/ir/learn/status"), 200,
           ["status", "target", "conflictWith"])
    expect("GET /api/system", d.get("/api/system"), 200,
           ["firmware", "chip", "uptimeMs", "resetReason", "heap", "psram", "fs", "ota", "wifi", "settings"])
    expect("GET /api/nonexistent -> 404", d.get("/api/nonexistent"), 404, ["error"], "not_found")
    return st if ok else None


# --------------------------------------------------------------------------
# [USB15] Плеєр USB: GET-перевірки (/api/usb/*). Нічого не змінюють.
# --------------------------------------------------------------------------
USB_STATUS_KEYS = ["active", "mode", "media", "cwd", "list", "queue", "nowPlaying", "lastReq", "pending"]
USB_MODE_STATES = ("Off", "Entering", "Ready", "Playing", "Exiting", "Error")
USB_MEDIA = ("none", "mounting", "mounted", "fault")  # usb_mode::mediaName(): малі літери
USB_QUEUE_STATES = ("Idle", "Starting", "Playing")
USB_LIST_MAX = 100


def usb_read_checks(d):
    """-> (usb_present, active). usb_present=False, якщо маршрутів нема (FEATURE_USB_MODE = 0)."""
    status, body = d.get("/api/usb/status")
    if status == 404:
        record("usb.*", False, "/api/usb/status -> 404: збірка без FEATURE_USB_MODE", skip=True)
        return False, False
    ok, st = expect("GET /api/usb/status", (status, body), 200, USB_STATUS_KEYS)
    if not ok:
        return True, False
    record("usb.status.mode.state / media допустимі",
           st["mode"].get("state") in USB_MODE_STATES and st["media"] in USB_MEDIA,
           "state=%r media=%r" % (st["mode"].get("state"), st["media"]))
    record("usb.status.list має valid/version/count/dirs/depth/truncated",
           has_keys(st["list"], ["valid", "version", "count", "dirs", "depth", "truncated"]))
    record("usb.status.queue має state/index/count/position/shuffle/repeat/paused/prepared/reason",
           has_keys(st["queue"], ["state", "index", "count", "position", "shuffle", "repeat", "paused", "prepared", "reason"])
           and st["queue"]["state"] in USB_QUEUE_STATES)
    record("usb.status.active == (state ∈ Ready/Playing)",
           st["active"] == (st["mode"].get("state") in ("Ready", "Playing")),
           "active=%r state=%r" % (st["active"], st["mode"].get("state")))
    if st["nowPlaying"] is not None:
        record("usb.status.nowPlaying має name/posSec/durSec/paused",
               has_keys(st["nowPlaying"], ["name", "posSec", "durSec", "paused"]))
    usb18a_shape_checks(st)  # [USB18a]
    if st["lastReq"] is not None:
        record("usb.status.lastReq має id/ok/why", has_keys(st["lastReq"], ["id", "ok", "why"]))

    # Параметри перевіряються ДО стану режиму: 400 однаково і з активним режимом, і без нього.
    expect("GET /api/usb/list?limit=0 -> 400", d.get("/api/usb/list?limit=0"), 400, ["error", "min", "max"], "out_of_range")
    expect("GET /api/usb/list?limit=%d -> 400" % (USB_LIST_MAX + 1), d.get("/api/usb/list?limit=%d" % (USB_LIST_MAX + 1)),
           400, ["error", "min", "max"], "out_of_range")
    expect("GET /api/usb/list?offset=abc -> 400", d.get("/api/usb/list?offset=abc"), 400, ["error", "field"], "invalid_value")
    expect("GET /api/usb/list?offset=-1 -> 400", d.get("/api/usb/list?offset=-1"), 400, ["error", "field"], "invalid_value")
    expect("GET /api/usb/result (без id) -> 400", d.get("/api/usb/result"), 400, ["error", "field"], "missing_field")
    expect("GET /api/usb/result?id=abc -> 400", d.get("/api/usb/result?id=abc"), 400, ["error", "field"], "invalid_value")
    expect("GET /api/usb/result?id=4000000000 -> 404", d.get("/api/usb/result?id=4000000000"), 404, ["error"], "unknown_req")
    expect("GET /api/usb/result?id=0 -> 404", d.get("/api/usb/result?id=0"), 404, ["error"], "unknown_req")
    expect("GET /api/usb/nonexistent -> 404", d.get("/api/usb/nonexistent"), 404, ["error"], "not_found")

    if not st["active"]:
        expect("GET /api/usb/list поза режимом -> 409 usb_mode_off", d.get("/api/usb/list"), 409, ["error", "state"],
               "usb_mode_off")
        return True, False

    # Режим активний: форма сторінки й повний обхід за nextOffset (нічого не змінює).
    if st["media"] != "mounted":
        expect("GET /api/usb/list без носія -> 409 no_media", d.get("/api/usb/list"), 409, ["error", "media"], "no_media")
        return True, True
    ok, pg = expect("GET /api/usb/list", d.get("/api/usb/list"), 200,
                    ["version", "cwd", "depth", "count", "dirs", "truncated", "offset", "limit", "items", "nextOffset"])
    if not ok:
        return True, True
    record("usb.list.items[] має i/name/dir",
           all(has_keys(it, ["i", "name", "dir"]) for it in pg["items"]))
    total, off, pages, bad_order, seen_dirs_after_file = 0, 0, 0, False, False
    cur = pg
    while True:
        pages += 1
        for k, it in enumerate(cur["items"]):
            if it["i"] != off + k:
                bad_order = True
            if it["dir"] and total + k >= cur["dirs"]:
                seen_dirs_after_file = True
        total += len(cur["items"])
        nxt = cur["nextOffset"]
        if nxt is None or pages > 200:
            break
        if nxt != off + len(cur["items"]):
            bad_order = True
            break
        off = nxt
        st2, cur = d.get("/api/usb/list?offset=%d&limit=%d&version=%d" % (off, USB_LIST_MAX, pg["version"]))
        if st2 != 200 or not isinstance(cur, dict):
            record("usb.list пагінація", st2 == 409, "HTTP %s (409 stale допустимий, якщо список змінився)" % st2)
            return True, True
    record("usb.list пагінація: сума записів == count (%d сторінок)" % pages, total == pg["count"] and not bad_order,
           "total=%d count=%d" % (total, pg["count"]))
    record("usb.list: папки перед файлами (dirs)", not seen_dirs_after_file)
    return True, True


# --------------------------------------------------------------------------
# Перевірки зі зміною стану (--write), кожна повертає початкове значення
# --------------------------------------------------------------------------
def write_checks(d, st, args):
    standby = bool(st and st.get("standby"))
    busy_mode = st and st.get("mode") in ("OtaUpdate", "IrLearn")
    if busy_mode:
        record("--write перевірки", False, "mode=%s: спершу завершіть OTA/навчання IR" % st.get("mode"), skip=True)
        return

    # --- негативні (без побічних ефектів) ---
    expect("POST /api/power {state:x} -> 400", d.post("/api/power", {"state": "x"}), 400, ["error"], "invalid_value")
    expect("POST /api/power {} -> 400", d.post("/api/power", {}), 400, ["error"])
    expect("POST /api/volume unknown field -> 400", d.post("/api/volume", {"vol": 1}), 400, ["error"], "unknown_field")
    expect("POST /api/volume value+step -> 400", d.post("/api/volume", {"value": 1, "step": 1}), 400, ["error"],
           "conflicting_fields")
    expect("POST /api/player/bogus -> 404", d.post("/api/player/bogus"), 404, ["error"], "not_found")
    expect("POST /api/ir/learn unknown action -> 400", d.post("/api/ir/learn", {"action": "NOPE"}), 400, ["error"])
    expect("POST /api/ir/learn ENC_CW (not learnable) -> 400", d.post("/api/ir/learn", {"action": "ENC_CW"}), 400,
           ["error"])
    expect("POST /api/ir/learn BASS_UP (зарезервовано) -> 400", d.post("/api/ir/learn", {"action": "BASS_UP"}), 400,
           ["error"])
    status, _ = d.post("/api/ota")
    record("POST /api/ota без тіла -> 4xx (не стартує)", 400 <= status < 500, "HTTP %s" % status)
    # --- P50a: видалене / нове (без побічних ефектів: валідація йде до застосування) ---
    expect("POST /api/gain -> 404 (маршрут видалено)", d.post("/api/gain", {"value": 1}), 404, ["error"], "not_found")
    for key in REMOVED_SETTINGS_KEYS:
        body = {key: "None" if key == "processorType" else 0}
        status, resp = d.post("/api/settings", body)
        codes = [e.get("code") for e in resp.get("errors", [])] if isinstance(resp, dict) else []
        record("POST /api/settings {%s} -> 400 unknown_field" % key,
               status == 400 and codes == ["unknown_field"], "HTTP %s %s" % (status, str(resp)[:80]))
    for cnt in (2, SLOT_COUNT + 1):  # [INP4b] приймається 3 (застаріло) або 4 елементи; інше -> expected_array_of_4
        status, resp = d.post("/api/settings", {"inputNames": ["a"] * cnt})
        codes = [e.get("code") for e in resp.get("errors", [])] if isinstance(resp, dict) else []
        record("POST /api/settings inputNames (%d елементи) -> 400 expected_array_of_4" % cnt,
               status == 400 and codes == ["expected_array_of_4"], "HTTP %s %s" % (status, str(resp)[:80]))
    expect("POST /api/balance {} -> 400 missing_field", d.post("/api/balance", {}), 400, ["error"], "missing_field")
    expect("POST /api/balance unknown field -> 400", d.post("/api/balance", {"value": 0, "x": 1}), 400, ["error"],
           "unknown_field")
    expect("POST /api/balance value=%d -> 400 out_of_range" % (BALANCE_MAX + 1),
           d.post("/api/balance", {"value": BALANCE_MAX + 1}), 400, ["error", "min", "max"], "out_of_range")
    expect("POST /api/balance input=%d -> 400 out_of_range" % SLOT_COUNT,
           d.post("/api/balance", {"value": 0, "input": SLOT_COUNT}), 400, ["error", "min", "max"], "out_of_range")

    # --- V2: POST /api/vu, негативні (валідація йде до застосування: побічних ефектів немає) ---
    expect("POST /api/vu {} -> 400 missing_field", d.post("/api/vu", {}), 400, ["error"], "missing_field")
    expect("POST /api/vu unknown field -> 400", d.post("/api/vu", {"value": 0, "x": 1}), 400, ["error"],
           "unknown_field")
    expect("POST /api/vu value=1.5 -> 400 invalid_value", d.post("/api/vu", {"value": 1.5}), 400, ["error"],
           "invalid_value")
    expect("POST /api/vu value=%d -> 400 out_of_range" % (VU_MAX + 1),
           d.post("/api/vu", {"value": VU_MAX + 1}), 400, ["error", "min", "max"], "out_of_range")
    expect("POST /api/vu value=%d -> 400 out_of_range" % (VU_MIN - 1),
           d.post("/api/vu", {"value": VU_MIN - 1}), 400, ["error", "min", "max"], "out_of_range")
    expect("POST /api/vu input=%d -> 400 out_of_range" % SLOT_COUNT,
           d.post("/api/vu", {"value": 0, "input": SLOT_COUNT}), 400, ["error", "min", "max"], "out_of_range")
    status, body = d.post("/api/vu", {"value": VU_MAX + 1})
    record("POST /api/vu out_of_range: field=value, min/max == %d/%d" % (VU_MIN, VU_MAX),
           status == 400 and isinstance(body, dict) and body.get("field") == "value" and
           body.get("min") == VU_MIN and body.get("max") == VU_MAX, "HTTP %s %s" % (status, str(body)[:80]))

    vmin, vmax = VOLUME_MIN, VOLUME_MAX
    expect("POST /api/volume above max -> 400", d.post("/api/volume", {"value": vmax + 1}), 400, ["error", "min", "max"],
           "out_of_range")
    bad_input = SLOT_COUNT
    status, body = d.post("/api/input", {"index": bad_input})
    record("POST /api/input {index:%d} -> 400 out_of_range, min/max == 0/%d" % (bad_input, SLOT_COUNT - 1),
           status == 400 and isinstance(body, dict) and body.get("error") == "out_of_range" and body.get("min") == 0 and
           body.get("max") == SLOT_COUNT - 1, "HTTP %s %s" % (status, str(body)[:80]))
    expect("POST /api/input {index:cur} (без зміни)", d.post("/api/input", {"index": st["input"]})
           if not standby else (409, {"error": "standby"}), 409 if standby else 200)

    # --- баланс неактивних входів (ВСІ слоти крім активного, у тому числі слот 3): дозволено й у standby (лише профіль) ---
    inactive = [i for i in range(SLOT_COUNT) if i != st["input"]]
    for other in inactive:
        status, o0 = d.get("/api/balance?input=%d" % other)
        if status == 200 and isinstance(o0, dict):
            b0 = o0["balance"]
            b1 = b0 + 1 if b0 < BALANCE_MAX else b0 - 1
            ok, _ = expect("POST /api/balance {value, input:%d} (неактивний)" % other,
                           d.post("/api/balance", {"value": b1, "input": other}), 200, ["input", "balance"])
            if ok:
                status, now = d.get("/api/balance?input=%d" % other)
                record("balance (неактивний %d): GET == %d" % (other, b1), status == 200 and now.get("balance") == b1, str(now))
            expect("POST /api/balance повернути %d (вхід %d)" % (b0, other),
                   d.post("/api/balance", {"value": b0, "input": other}), 200, ["input", "balance"])
            status, now = d.get("/api/balance?input=%d" % other)
            record("balance (неактивний %d): після повернення == %d" % (other, b0), status == 200 and now.get("balance") == b0, str(now))
        else:
            record("balance неактивного входу %d" % other, False, "не вдалося прочитати", skip=True)

    # --- V2: чутливість VU неактивних входів (ВСІ слоти крім активного, у тому числі слот 3): дозволено й у standby ---
    status, w0 = d.get("/api/vu")
    if status == 200 and isinstance(w0, dict) and isinstance(w0.get("inputs"), list) and len(w0["inputs"]) == SLOT_COUNT:
        for vu_other in [i for i in range(SLOT_COUNT) if i != w0["input"]]:
            u0 = w0["inputs"][vu_other]["sensitivityDb"]
            u1 = u0 + 1 if u0 < VU_MAX else u0 - 1
            ok, b = expect("POST /api/vu {value, input:%d} (неактивний)" % vu_other,
                           d.post("/api/vu", {"value": u1, "input": vu_other}), 200, ["input", "sensitivityDb"])
            if ok:
                record("vu (неактивний %d): відповідь input == %d, sensitivityDb == %d" % (vu_other, vu_other, u1),
                       b["input"] == vu_other and b["sensitivityDb"] == u1, str(b))
                status, now = d.get("/api/vu")
                record("vu (неактивний %d): GET inputs[%d] == %d" % (vu_other, vu_other, u1),
                       status == 200 and now["inputs"][vu_other]["sensitivityDb"] == u1, str(now)[:100])
                if status == 200:
                    others_ok = all(now["inputs"][i]["sensitivityDb"] == w0["inputs"][i]["sensitivityDb"]
                                    for i in range(SLOT_COUNT) if i != vu_other)
                    record("vu (неактивний %d): інші входи не змінились" % vu_other, others_ok, str(now["inputs"])[:100])
            expect("POST /api/vu повернути %d (вхід %d)" % (u0, vu_other),
                   d.post("/api/vu", {"value": u0, "input": vu_other}), 200, ["input", "sensitivityDb"])
            status, now = d.get("/api/vu")
            record("vu (неактивний %d): після повернення == %d" % (vu_other, u0),
                   status == 200 and now["inputs"][vu_other]["sensitivityDb"] == u0, str(now)[:100])
    else:
        record("vu неактивного входу", False, "не вдалося прочитати", skip=True)

    # --- [INP4b] слот 3 («Player» / USB): маска, ім'я, EQ (режим «USB» не активний) ---
    slot3_checks(d, st, standby)

    if standby:
        record("volume/mute/balance(активний) зміни", False, "пристрій у standby (очікувано 409)", skip=True)
        expect("POST /api/volume у standby -> 409", d.post("/api/volume", {"value": st["volume"]}), 409, ["error"], "standby")
        expect("POST /api/balance (активний) у standby -> 409", d.post("/api/balance", {"value": st["balance"]}), 409,
               ["error"], "standby")
        expect("POST /api/vu (активний) у standby -> 409", d.post("/api/vu", {"value": 0}), 409, ["error"], "standby")
    else:
        # --- гучність ---
        v0 = st["volume"]
        v1 = v0 + 1 if v0 < vmax else v0 - 1
        ok, b = expect("POST /api/volume {value} змінює гучність", d.post("/api/volume", {"value": v1}), 200, ["state"])
        if ok:
            record("volume: state.volume == %d" % v1, b["state"]["volume"] == v1, "got %s" % b["state"]["volume"])
        ok, b = expect("POST /api/volume {step:-1}", d.post("/api/volume", {"step": -1}), 200, ["state"])
        expect("POST /api/volume повернути %d" % v0, d.post("/api/volume", {"value": v0}), 200, ["state"])

        # --- мʼют ---
        m0 = st["mute"]
        ok, b = expect("POST /api/mute {mute:!cur}", d.post("/api/mute", {"mute": not m0}), 200, ["state"])
        if ok:
            record("mute: state.mute == %s" % (not m0), b["state"]["mute"] == (not m0))
        expect("POST /api/mute {mute:toggle}", d.post("/api/mute", {"mute": "toggle"}), 200, ["state"])
        expect("POST /api/mute повернути %s" % m0, d.post("/api/mute", {"mute": m0}), 200, ["state"])
        expect("POST /api/mute invalid -> 400", d.post("/api/mute", {"mute": 5}), 400, ["error"], "invalid_value")

        # --- баланс активного входу (через AppController) ---
        c0 = st["balance"]
        c1 = c0 + 1 if c0 < BALANCE_MAX else c0 - 1
        ok, b = expect("POST /api/balance {value} (активний вхід)", d.post("/api/balance", {"value": c1}), 200,
                       ["input", "balance"])
        if ok:
            record("balance: відповідь input == status.input, balance == %d" % c1,
                   b["input"] == st["input"] and b["balance"] == c1, str(b))
            status, now = d.get("/api/status")
            record("balance: /api/status.balance == %d" % c1, status == 200 and now.get("balance") == c1,
                   "got %s" % (now.get("balance") if isinstance(now, dict) else now))
            status, now = d.get("/api/balance")
            record("balance: GET /api/balance == %d" % c1, status == 200 and now.get("balance") == c1, str(now))
        expect("POST /api/balance повернути %d" % c0, d.post("/api/balance", {"value": c0}), 200, ["input", "balance"])

        # --- V2: чутливість VU активного входу (через AppController) ---
        k0 = st["vuSens"]
        k1 = k0 + 1 if k0 < VU_MAX else k0 - 1
        ok, b = expect("POST /api/vu {value} (активний вхід)", d.post("/api/vu", {"value": k1}), 200,
                       ["input", "sensitivityDb"])
        if ok:
            record("vu: відповідь input == status.input, sensitivityDb == %d" % k1,
                   b["input"] == st["input"] and b["sensitivityDb"] == k1, str(b))
            status, now = d.get("/api/status")
            record("vu: /api/status.vuSens == %d" % k1, status == 200 and now.get("vuSens") == k1,
                   "got %s" % (now.get("vuSens") if isinstance(now, dict) else now))
            status, now = d.get("/api/vu")
            record("vu: GET /api/vu inputs[active] == %d" % k1,
                   status == 200 and now["inputs"][st["input"]]["sensitivityDb"] == k1, str(now)[:100])
        expect("POST /api/vu повернути %d" % k0, d.post("/api/vu", {"value": k0}), 200, ["input", "sensitivityDb"])
        status, now = d.get("/api/vu")
        record("vu: після повернення inputs[active] == %d" % k0,
               status == 200 and now["inputs"][st["input"]]["sensitivityDb"] == k0, str(now)[:100])

    # --- яскравість (не залежить від standby) ---
    br0 = st["brightness"]
    br1 = br0 - 1 if br0 > 0 else br0 + 1
    expect("POST /api/settings {brightness}", d.post("/api/settings", {"brightness": br1}), 200, ["ok", "results"])
    expect("POST /api/settings повернути brightness", d.post("/api/settings", {"brightness": br0}), 200, ["ok"])
    status, body = d.post("/api/settings", {"brightness": 101})
    record("POST /api/settings brightness=101 -> 400", status == 400 and body.get("ok") is False
           and body["errors"][0]["code"] == "out_of_range", "HTTP %s" % status)

    # --- станції: тимчасова станція додається й видаляється ---
    status, before = d.get("/api/stations")
    if status == 200 and isinstance(before, list):
        n0 = len(before)
        full = st and n0 >= st["station"].get("max", 10**9)
        if full:
            record("stations add/delete roundtrip", False, "список заповнений", skip=True)
        else:
            ok, b = expect("POST /api/stations (add __smoke__)",
                           d.post("/api/stations", {"name": "__smoke__", "url": "http://127.0.0.1/smoke"}), 201, ["index"])
            if ok:
                idx = b["index"]
                status, after = d.get("/api/stations")
                record("add: count +1, остання = __smoke__", status == 200 and len(after) == n0 + 1
                       and after[idx]["name"] == "__smoke__")
                expect("DELETE /api/stations/%d" % idx, d.call("DELETE", "/api/stations/%d" % idx), 200, ["ok"])
                status, after = d.get("/api/stations")
                record("delete: count == початковий", status == 200 and len(after) == n0)
        expect("POST /api/stations bad scheme -> 400",
               d.post("/api/stations", {"name": "x", "url": "ftp://x"}), 400, ["error"], "url_invalid_scheme")
        expect("POST /api/player/station index out of range -> 400",
               d.post("/api/player/station", {"index": n0 + 1000}), 400, ["error"], "index_out_of_range")
    else:
        record("stations roundtrip", False, "не вдалося прочитати список", skip=True)

    # --- плеєр: чутні переходи, лише з --player ---
    if args.player:
        player_checks(d, st)
    else:
        record("player play/pause/next/prev", False, "додайте --player", skip=True)

    # --- підтвердження небезпечних дій ---
    if args.confirm_checks:
        expect("POST /api/system/factory-reset {} -> 400", d.post("/api/system/factory-reset", {}), 400, ["error"],
               "confirm_required")
        expect("POST /api/system/factory-reset confirm:false -> 400",
               d.post("/api/system/factory-reset", {"confirm": False}), 400, ["error"], "confirm_required")
        expect("POST /api/wifi/reset {} -> 400", d.post("/api/wifi/reset", {}), 400, ["error"], "confirm_required")
    else:
        record("confirm-перевірки reset", False, "додайте --confirm-checks", skip=True)


# --------------------------------------------------------------------------
# [INP4b] Вхід-слот 3 («Player» / USB): допоміжні функції, перевірки в неактивному стані й у режимі «USB»
# --------------------------------------------------------------------------
def get_inputs(d):
    """GET /api/inputs -> тіло (dict) з рівно SLOT_COUNT входами або None."""
    status, body = d.get("/api/inputs")
    if status == 200 and isinstance(body, dict) and isinstance(body.get("inputs"), list) \
            and len(body["inputs"]) == SLOT_COUNT:
        return body
    return None


def wait_until(fn, timeout=10.0, step=0.4):
    """Викликає fn() до першого «істинного» результату (його й повертає) або до таймауту (-> None)."""
    end = time.time() + timeout
    while True:
        try:
            v = fn()
        except Exception:  # noqa: BLE001 - тимчасова відсутність поля тощо
            v = None
        if v:
            return v
        if time.time() >= end:
            return None
        time.sleep(step)


def usb_active_body(d):
    """GET /api/usb/status -> тіло, якщо режим «USB» активний (active == true), інакше None."""
    status, body = d.get("/api/usb/status")
    if status == 200 and isinstance(body, dict) and body.get("active") is True:
        return body
    return None


def usb_inactive_body(d):
    """GET /api/usb/status -> тіло, якщо режим повністю вимкнено (mode.state == "Off"), інакше None.
    (active == false ще й під час Exiting, тому лише active для «вийшли» недостатньо.)"""
    status, body = d.get("/api/usb/status")
    if status == 200 and isinstance(body, dict) and body.get("active") is False and \
            isinstance(body.get("mode"), dict) and body["mode"].get("state") == "Off":
        return body
    return None


def status_where(d, pred):
    """GET /api/status -> тіло, якщо pred(тіло) істинний, інакше None."""
    status, body = d.get("/api/status")
    if status == 200 and isinstance(body, dict) and pred(body):
        return body
    return None


def post_inputs_slot3(d, value, form):
    """Змінює слот 3 в /api/inputs: form 'index' — {index:3,enabled:v}; 'array4' — [null,null,null,v]."""
    if form == "index":
        return d.post("/api/inputs", {"index": USB_SLOT, "enabled": value})
    return d.post("/api/inputs", {"enabled": [None, None, None, value]})


def slot3_checks(d, st, standby):
    """[INP4b] Слот 3 у НЕАКТИВНОМУ стані (режим «USB» вимкнено): маска, ім'я, профіль EQ. Усе повертається.
    (Баланс і VU слота 3 перевіряються в циклах «неактивних входів» у write_checks.)"""
    inp0 = get_inputs(d)
    status, sett0 = d.get("/api/settings")
    if inp0 is None or status != 200 or not isinstance(sett0, dict) or \
            not isinstance(sett0.get("inputNames"), list) or len(sett0["inputNames"]) != SLOT_COUNT:
        record("слот 3 (USB): маска / ім'я / EQ", False, "не вдалося прочитати /api/inputs або /api/settings", skip=True)
        return
    s3 = inp0["inputs"][USB_SLOT]
    if not s3.get("available"):
        record("слот 3 (USB): маска / ім'я / EQ", False, "inputs[3].available=false (збірка без USB-входу)", skip=True)
        expect("POST /api/inputs {index:3,enabled:true} без USB-входу -> 400 input_unavailable",
               d.post("/api/inputs", {"index": USB_SLOT, "enabled": True}), 400, ["error"], "input_unavailable")
        return
    en0, mask0 = bool(s3["enabled"]), inp0["enabledMask"]
    names0 = list(sett0["inputNames"])
    eq_status, eq0 = d.get("/api/eq?input=%d" % USB_SLOT)
    eq0_gains = eq0.get("gainsDb") if eq_status == 200 and isinstance(eq0, dict) else None
    try:
        # (а) вимкнення слота 3 (форма {index, enabled}); відповідь повертає повний стан
        ok, b = expect("POST /api/inputs {index:3,enabled:false}", post_inputs_slot3(d, False, "index"), 200,
                       ["inputs", "enabledMask", "input"])
        if ok:
            s = b["inputs"][USB_SLOT]
            record("слот 3 вимкнено: enabled=false, selectable=false, біт 3 маски знято, біти 0..2 не змінились",
                   s["enabled"] is False and s["selectable"] is False and (b["enabledMask"] & 8) == 0 and
                   (b["enabledMask"] & 7) == (mask0 & 7), str(b)[:120])
        g = get_inputs(d)
        record("слот 3 вимкнено: GET /api/inputs збігається", g is not None and g["inputs"][USB_SLOT]["enabled"] is False)
        if standby:
            record("POST /api/input {index:3} при вимкненому слоті -> 409 input_disabled", False,
                   "пристрій у standby (там 409 standby)", skip=True)
        else:
            expect("POST /api/input {index:3} при вимкненому слоті -> 409 input_disabled",
                   d.post("/api/input", {"index": USB_SLOT}), 409, ["error"], "input_disabled")
        # (б) увімкнення слота 3 (форма масиву з 4 елементів)
        ok, b = expect("POST /api/inputs {enabled:[null,null,null,true]} (4 елементи)", post_inputs_slot3(d, True, "array4"),
                       200, ["inputs", "enabledMask", "input"])
        if ok:
            record("слот 3 увімкнено: enabled=true, selectable=true, біт 3 маски",
                   b["inputs"][USB_SLOT]["enabled"] is True and b["inputs"][USB_SLOT]["selectable"] is True and
                   (b["enabledMask"] & 8) == 8, str(b)[:120])
        # (в) масив із 3 елементів (ЗАСТАРІЛО) НЕ чіпає слот 3: поточні значення входів 1..2 передаємо без змін
        cur = get_inputs(d)
        if cur is not None:
            e1, e2 = cur["inputs"][1]["enabled"], cur["inputs"][2]["enabled"]
            for want3 in (True, False):
                post_inputs_slot3(d, want3, "index")
                ok, b = expect("POST /api/inputs {enabled:[null,%s,%s]} (3 елементи, застаріло), слот 3 == %s"
                               % (str(e1).lower(), str(e2).lower(), want3),
                               d.post("/api/inputs", {"enabled": [None, e1, e2]}), 200, ["inputs", "enabledMask"])
                if ok:
                    record("3 елементи: слот 3 не змінився (enabled == %s), входи 1..2 без змін" % want3,
                           b["inputs"][USB_SLOT]["enabled"] is want3 and b["inputs"][1]["enabled"] is e1 and
                           b["inputs"][2]["enabled"] is e2, str(b)[:120])
        expect("POST /api/inputs {enabled:[null,true]} (2 елементи) -> 400 invalid_value",
               d.post("/api/inputs", {"enabled": [None, True]}), 400, ["error", "field"], "invalid_value")
        expect("POST /api/inputs {enabled: 5 елементів} -> 400 invalid_value",
               d.post("/api/inputs", {"enabled": [None] * (SLOT_COUNT + 1)}), 400, ["error", "field"], "invalid_value")
        expect("POST /api/inputs {index:%d,enabled:true} -> 400 out_of_range" % SLOT_COUNT,
               d.post("/api/inputs", {"index": SLOT_COUNT, "enabled": True}), 400, ["error", "min", "max"], "out_of_range")

        # (г) імена: слот 3 змінюється лише 4-елементним масивом; 3 елементи його не чіпають
        test_name = "Плеєр-смоук"
        ok, _ = expect("POST /api/settings {inputNames:[null,null,null,\"%s\"]} (4 елементи)" % test_name,
                       d.post("/api/settings", {"inputNames": [None, None, None, test_name]}), 200, ["ok", "results"])
        status, sett = d.get("/api/settings")
        record("ім'я слота 3 == %r, імена 0..2 без змін" % test_name,
               status == 200 and sett["inputNames"][USB_SLOT] == test_name and sett["inputNames"][:3] == names0[:3],
               str(sett.get("inputNames") if isinstance(sett, dict) else sett)[:100])
        g = get_inputs(d)
        status, sst = d.get("/api/status")
        record("ім'я слота 3 видно в /api/inputs і /api/status.inputs",
               g is not None and g["inputs"][USB_SLOT]["name"] == test_name and status == 200 and
               isinstance(sst, dict) and sst["inputs"][USB_SLOT]["name"] == test_name)
        ok, _ = expect("POST /api/settings {inputNames: 3 елементи (поточні), застаріло}",
                       d.post("/api/settings", {"inputNames": names0[:3]}), 200, ["ok", "results"])
        status, sett = d.get("/api/settings")
        record("3 елементи inputNames: слот 3 не змінився (%r)" % test_name,
               status == 200 and sett["inputNames"][USB_SLOT] == test_name and sett["inputNames"][:3] == names0[:3],
               str(sett.get("inputNames") if isinstance(sett, dict) else sett)[:100])
        status, body = d.post("/api/settings", {"inputNames": [None, None, None, "x" * 32]})
        errs = body.get("errors") if isinstance(body, dict) else None
        record("POST /api/settings ім'я слота 3 > 31 байта -> 400 invalid_name (index 3)",
               status == 400 and isinstance(errs, list) and len(errs) == 1 and errs[0].get("code") == "invalid_name" and
               errs[0].get("index") == USB_SLOT, "HTTP %s %s" % (status, str(body)[:100]))

        # (д) EQ слота 3 (профіль «Player»; режим не активний -> лише збереження), вхід 0 не змінюється
        if eq0_gains is not None and isinstance(eq0.get("minDb"), int):
            st0, e_in0 = d.get("/api/eq?input=0")
            g0 = list(eq0_gains)
            g1 = g0[0] + 1 if g0[0] < eq0["maxDb"] else g0[0] - 1
            ok, b = expect("POST /api/eq {input:3,band:0,gainDb:%d} (профіль «Player»)" % g1,
                           d.post("/api/eq", {"input": USB_SLOT, "band": 0, "gainDb": g1}), 200, ["input", "gainsDb"])
            if ok:
                record("eq слота 3: відповідь input == 3, gainsDb[0] == %d, решта без змін" % g1,
                       b["input"] == USB_SLOT and b["gainsDb"][0] == g1 and b["gainsDb"][1:] == g0[1:], str(b)[:100])
            status, now = d.get("/api/eq?input=%d" % USB_SLOT)
            record("eq слота 3: GET == збережене", status == 200 and now.get("gainsDb", [None])[0] == g1, str(now)[:100])
            status, e_in0b = d.get("/api/eq?input=0")
            record("eq слота 3: профіль входу 0 не змінився",
                   st0 == 200 and status == 200 and e_in0.get("gainsDb") == e_in0b.get("gainsDb"), str(e_in0b)[:100])
            expect("POST /api/eq {input:3,gainsDb:[...]} повернути", d.post("/api/eq", {"input": USB_SLOT, "gainsDb": g0}),
                   200, ["input", "gainsDb"])
            status, now = d.get("/api/eq?input=%d" % USB_SLOT)
            record("eq слота 3: після повернення == початкове", status == 200 and now.get("gainsDb") == g0, str(now)[:100])
        else:
            record("eq слота 3", False, "не вдалося прочитати GET /api/eq?input=3", skip=True)
        expect("POST /api/eq {input:%d,band:0,gainDb:0} -> 400 out_of_range" % SLOT_COUNT,
               d.post("/api/eq", {"input": SLOT_COUNT, "band": 0, "gainDb": 0}), 400, ["error", "min", "max"], "out_of_range")
    finally:
        # --- відновлення: ім'я, EQ (на випадок збою посередині), маска слота 3 ---
        d.post("/api/settings", {"inputNames": [None, None, None, names0[USB_SLOT]]})
        if eq0_gains is not None:
            d.post("/api/eq", {"input": USB_SLOT, "gainsDb": eq0_gains})
        post_inputs_slot3(d, en0, "array4")
        g = get_inputs(d)
        status, sett = d.get("/api/settings")
        record("слот 3 відновлено: enabledMask == %d, ім'я == %r" % (mask0, names0[USB_SLOT]),
               g is not None and g["enabledMask"] == mask0 and status == 200 and
               sett["inputNames"][USB_SLOT] == names0[USB_SLOT],
               "mask=%s name=%s" % (g["enabledMask"] if g else None, sett["inputNames"][USB_SLOT] if status == 200 else None))


def usb_leave(d, prev, vol0, bal0, vu0):
    """Вихід із режиму «USB» на вхід prev + перевірка, що профіль входу prev відновився. Викликається завжди (finally)."""
    act = usb_active_body(d)
    cur = status_where(d, lambda s: s.get("input") == USB_SLOT)
    if act is None and cur is None:
        return  # режим не було піднято (або вже вийшли)
    status, b = d.post("/api/input", {"index": prev})
    record("POST /api/input {index:%d} з режиму «USB» -> 202 usb_exit" % prev,
           status == 202 and isinstance(b, dict) and b.get("ok") is True and b.get("pending") is True and
           b.get("reason") == "usb_exit" and b.get("target") == prev, "HTTP %s %s" % (status, str(b)[:120]))
    off = wait_until(lambda: usb_inactive_body(d), 15)
    record("вихід: /api/usb/status mode.state == Off", bool(off), "" if off else "режим не вимкнувся за 15 с")
    back = wait_until(lambda: status_where(d, lambda s: s.get("input") == prev), 8)
    record("вихід: status.input == %d" % prev, bool(back), "" if back else "input не повернувся")
    prof = wait_until(lambda: status_where(
        d, lambda s: s.get("input") == prev and s.get("volume") == vol0 and s.get("balance") == bal0 and
        s.get("vuSens") == vu0), 8)
    record("вихід: профіль входу %d відновлено (volume %s, balance %s, vuSens %s)" % (prev, vol0, bal0, vu0), bool(prof),
           "" if prof else "після виходу інші значення")


def usb_input_checks(d, st):
    """[INP4b] Режим «USB» через слот 3 (ЧУТНІ переходи; лише з --write --player). Вхід: POST /api/input 3; вихід — на
    попередній вхід. Усе, що змінюється всередині (EQ / баланс / VU / гучність «Player»), повертається. Якщо слот 3 вимкнено
    в налаштуваннях — на час перевірки він тимчасово вмикається й після виходу з режиму повертається вимкненим."""
    if not st or st.get("standby") or st.get("mode") not in ("Radio", "ExternalInput"):
        record("режим «USB» через вхід 3", False, "потрібен увімкнений пристрій у режимі Radio / ExternalInput", skip=True)
        return
    inp = get_inputs(d)
    if inp is None or not inp["inputs"][USB_SLOT].get("available"):
        record("режим «USB» через вхід 3", False, "слот 3 недоступний (збірка без USB-входу)", skip=True)
        return
    temp_enabled = False
    if not inp["inputs"][USB_SLOT].get("enabled"):
        ok, _ = expect("POST /api/inputs {enabled:[null,null,null,true]} (слот 3 тимчасово вмикається для перевірки режиму)",
                       post_inputs_slot3(d, True, "array4"), 200, ["inputs", "enabledMask"])
        if not ok:
            return
        temp_enabled = True
        inp = get_inputs(d)
        if inp is None:
            return
    try:
        _usb_input_checks_body(d, st, inp)
    finally:
        if temp_enabled:
            post_inputs_slot3(d, False, "array4")
            g = get_inputs(d)
            record("слот 3 повернуто вимкненим після перевірки режиму «USB» (enabledMask == %d)" % (inp["enabledMask"] & ~8),
                   g is not None and g["inputs"][USB_SLOT]["enabled"] is False and g["enabledMask"] == (inp["enabledMask"] & ~8),
                   str(g.get("enabledMask") if g else None))


def _usb_input_checks_body(d, st, inp):
    status, s0 = d.get("/api/status")
    if status != 200 or not isinstance(s0, dict):
        record("режим «USB» через вхід 3", False, "не вдалося прочитати /api/status", skip=True)
        return
    prev, vol0, bal0, vu0 = s0["input"], s0["volume"], s0["balance"], s0["vuSens"]
    status, eq3 = d.get("/api/eq?input=%d" % USB_SLOT)
    eq3_0 = list(eq3["gainsDb"]) if status == 200 and isinstance(eq3, dict) and "gainsDb" in eq3 else None
    status, vu_before = d.get("/api/vu")
    try:
        status, b = d.post("/api/input", {"index": USB_SLOT})
        record("POST /api/input {index:3} -> 202 usb_enter",
               status == 202 and isinstance(b, dict) and b.get("ok") is True and b.get("pending") is True and
               b.get("reason") == "usb_enter" and b.get("target") == USB_SLOT and isinstance(b.get("state"), dict),
               "HTTP %s %s" % (status, str(b)[:120]))
        if status != 202:
            return
        up = wait_until(lambda: usb_active_body(d), 15)
        record("після входу: /api/usb/status.active == true", bool(up), "" if up else "режим не піднявся за 15 с")
        if not up:
            return
        su = wait_until(lambda: status_where(d, lambda s: s.get("input") == USB_SLOT), 6)
        record("у режимі «USB»: status.input == 3", bool(su), "" if su else "status.input != 3")
        if su:
            record("у режимі «USB»: status.inputName == назва слота 3 (/api/inputs)",
                   su.get("inputName") == get_inputs(d)["inputs"][USB_SLOT]["name"], str(su.get("inputName")))
        gi = get_inputs(d)
        record("у режимі «USB»: /api/inputs.input == 3", gi is not None and gi.get("input") == USB_SLOT)
        for path in ("/api/balance", "/api/vu", "/api/eq"):
            status, body = d.get(path)
            record("у режимі «USB»: GET %s (без input) -> input == 3" % path,
                   status == 200 and isinstance(body, dict) and body.get("input") == USB_SLOT, str(body)[:80])

        status, b = d.post("/api/input", {"index": USB_SLOT})
        record("повторний POST /api/input {index:3} у режимі -> 200 без pending",
               status == 200 and isinstance(b, dict) and b.get("ok") is True and "pending" not in b,
               "HTTP %s %s" % (status, str(b)[:100]))

        # --- input_active: вимкнення слота 3 у режимі «USB» заборонено, маска не змінюється ---
        expect("POST /api/inputs {index:3,enabled:false} у режимі «USB» -> 409 input_active",
               post_inputs_slot3(d, False, "index"), 409, ["error"], "input_active")
        expect("POST /api/inputs {enabled:[null,null,null,false]} у режимі «USB» -> 409 input_active",
               post_inputs_slot3(d, False, "array4"), 409, ["error"], "input_active")
        gi = get_inputs(d)
        record("input_active: слот 3 лишився enabled, маска не змінилась",
               gi is not None and gi["inputs"][USB_SLOT]["enabled"] is True and gi["enabledMask"] == inp["enabledMask"],
               str(gi.get("enabledMask") if gi else None))

        # --- EQ «Player» у режимі (живе застосування; чутно) і незалежність від входу prev ---
        if eq3_0 is not None and isinstance(eq3.get("maxDb"), int):
            g1 = eq3_0[0] + 1 if eq3_0[0] < eq3["maxDb"] else eq3_0[0] - 1
            ok, b = expect("POST /api/eq {band:0,gainDb:%d} у режимі (без input -> слот 3)" % g1,
                           d.post("/api/eq", {"band": 0, "gainDb": g1}), 200, ["input", "gainsDb"])
            if ok:
                record("eq у режимі: відповідь input == 3, gainsDb[0] == %d" % g1,
                       b["input"] == USB_SLOT and b["gainsDb"][0] == g1, str(b)[:100])
            d.post("/api/eq", {"input": USB_SLOT, "gainsDb": eq3_0})
            status, now = d.get("/api/eq?input=%d" % USB_SLOT)
            record("eq у режимі: повернуто початковий пресет «Player»", status == 200 and now.get("gainsDb") == eq3_0)
        else:
            record("eq у режимі «USB»", False, "не вдалося прочитати EQ слота 3", skip=True)

        # --- баланс / VU: без input -> слот 3; профіль входу prev не змінюється ---
        status, bu = d.get("/api/balance")
        status_s, su0 = d.get("/api/status")
        if status == 200 and status_s == 200 and isinstance(bu, dict):
            b0 = bu["balance"]
            b1 = b0 + 1 if b0 < BALANCE_MAX else b0 - 1
            ok, b = expect("POST /api/balance {value:%d} у режимі (без input -> слот 3)" % b1,
                           d.post("/api/balance", {"value": b1}), 200, ["input", "balance"])
            if ok:
                record("balance у режимі: відповідь input == 3, balance == %d" % b1,
                       b["input"] == USB_SLOT and b["balance"] == b1, str(b))
                status, now = d.get("/api/status")
                record("balance у режимі: /api/status.balance == %d" % b1, status == 200 and now.get("balance") == b1)
                status, other = d.get("/api/balance?input=%d" % prev)
                record("balance у режимі: профіль входу %d не змінився (%d)" % (prev, bal0),
                       status == 200 and other.get("balance") == bal0, str(other))
            expect("POST /api/balance повернути %d (слот 3)" % b0, d.post("/api/balance", {"value": b0}), 200,
                   ["input", "balance"])
        if status_s == 200 and isinstance(su0, dict):
            k0 = su0["vuSens"]
            k1 = k0 + 1 if k0 < VU_MAX else k0 - 1
            ok, b = expect("POST /api/vu {value:%d} у режимі (без input -> слот 3)" % k1,
                           d.post("/api/vu", {"value": k1}), 200, ["input", "sensitivityDb"])
            if ok:
                record("vu у режимі: відповідь input == 3, sensitivityDb == %d" % k1,
                       b["input"] == USB_SLOT and b["sensitivityDb"] == k1, str(b))
                status, now = d.get("/api/vu")
                if status == 200 and isinstance(vu_before, dict) and isinstance(vu_before.get("inputs"), list):
                    record("vu у режимі: inputs[3] == %d, inputs[%d] без змін" % (k1, prev),
                           now["inputs"][USB_SLOT]["sensitivityDb"] == k1 and
                           now["inputs"][prev]["sensitivityDb"] == vu_before["inputs"][prev]["sensitivityDb"],
                           str(now.get("inputs"))[:100])
            expect("POST /api/vu повернути %d (слот 3)" % k0, d.post("/api/vu", {"value": k0}), 200, ["input", "sensitivityDb"])

        # --- гучність «Player» і lastInput / lastVolume ---
        status, sv = d.get("/api/status")
        if status == 200 and isinstance(sv, dict):
            v0 = sv["volume"]
            v1 = v0 + 1 if v0 < VOLUME_MAX else v0 - 1
            ok, b = expect("POST /api/volume {value:%d} у режимі «USB» (гучність «Player»)" % v1,
                           d.post("/api/volume", {"value": v1}), 200, ["state"])
            if ok:
                record("volume у режимі: state.input == 3, state.volume == %d" % v1,
                       b["state"].get("input") == USB_SLOT and b["state"].get("volume") == v1, str(b)[:100])
            sett = wait_until(lambda: (lambda r: r[1] if r[0] == 200 and isinstance(r[1], dict) and
                                       r[1].get("lastInput") == USB_SLOT and r[1].get("lastVolume") == v1 else None)(
                d.get("/api/settings")), 10)
            record("settings у режимі: lastInput == 3, lastVolume == %d (профіль «Player», не радіо)" % v1, bool(sett),
                   "" if sett else "lastInput / lastVolume не збіглись за 10 с")
            expect("POST /api/volume повернути %d («Player»)" % v0, d.post("/api/volume", {"value": v0}), 200, ["state"])
            back = wait_until(lambda: status_where(d, lambda s: s.get("volume") == v0), 6)
            record("volume «Player» відновлено (%d)" % v0, bool(back))
        usb18a_track_checks(d)  # [USB18a] чутне відтворення: path / теги / next / seek за кінець
    except Exception as exc:  # noqa: BLE001
        record("режим «USB» через вхід 3: неочікуваний виняток", False, repr(exc))
    finally:
        usb_leave(d, prev, vol0, bal0, vu0)


# --------------------------------------------------------------------------
# [USB18a] Нові поля status: nowPlaying.path/dir/title/artist/album, queue.pos; відтворення, теги, seek за кінець
# --------------------------------------------------------------------------
def parent_dir(path):
    i = path.rfind("/")
    return "/" if i <= 0 else path[:i]


def usb18a_shape_checks(st, label="usb18a"):
    """Типи нових полів у відповіді GET /api/usb/status (st — розібране тіло). Нічого не змінює."""
    q = st.get("queue") if isinstance(st, dict) else None
    record("%s: queue.pos — ціле >= 0" % label,
           isinstance(q, dict) and isinstance(q.get("pos"), int) and not isinstance(q.get("pos"), bool) and q["pos"] >= 0,
           "pos=%r" % (q.get("pos") if isinstance(q, dict) else None))
    np = st.get("nowPlaying")
    if np is None:
        record("%s: nowPlaying == null -> path/dir/теги відсутні" % label, True)
        return
    record("%s: nowPlaying.path — рядок від кореня, закінчується на name" % label,
           isinstance(np.get("path"), str) and np["path"].startswith("/") and isinstance(np.get("name"), str) and
           np["path"].endswith(np["name"]), "path=%r name=%r" % (np.get("path"), np.get("name")))
    record("%s: nowPlaying.dir == батьківська папка path" % label,
           isinstance(np.get("dir"), str) and isinstance(np.get("path"), str) and np["dir"] == parent_dir(np["path"]),
           "dir=%r" % np.get("dir"))
    bad = [k for k in ("title", "artist", "album") if k in np and not (isinstance(np[k], str) and np[k] != "")]
    record("%s: теги title/artist/album — непорожні рядки або відсутні" % label, not bad, "некоректні: %s" % ", ".join(bad))


def usb_status_body(d):
    status, body = d.get("/api/usb/status")
    return body if status == 200 and isinstance(body, dict) else None


def wait_track_change(d, old_path, timeout=10):
    """Чекає, поки nowPlaying.path стане іншим (трек стартував) -> тіло status; або None."""
    def probe():
        b = usb_status_body(d)
        np = b.get("nowPlaying") if b else None
        return b if np and np.get("path") and np["path"] != old_path else None
    return wait_until(probe, timeout, 0.4)


def usb_settled(d, timeout=5):
    """Тіло status, коли трек уже Playing, тривалість відома (durSec > 0) і немає паузи (короткі файли: дії одразу після next
    потрапляють у фазу старту, коли durSec == 0 і пауза / seek відхиляються). None — не дочекались."""
    def probe():
        b = usb_status_body(d)
        n = b.get("nowPlaying") if b else None
        return b if n and b["queue"].get("state") == "Playing" and (n.get("durSec") or 0) > 0 and not n.get("paused") else None
    return wait_until(probe, timeout, 0.2)


def usb18a_track_checks(d):
    """[USB18a] Реальне відтворення (ЧУТНЕ; лише з --write --player, у режимі «USB»): path / dir / теги / pos, next, seek,
    seek за кінець, повернення до початкового треку. Режим «USB» і профіль «Player» повертає зовнішній код (usb_leave)."""
    # Після входу в режим пристрій сам відновлює останню папку й трек (автостарт, §16.2): список може змінитись під ногами.
    # Тому чекаємо, поки cwd / list.version стабільні, і запускаємо перший файл з повторами (до 3 спроб).
    def stable_status():
        seen, same, end = None, 0, time.time() + 12
        while time.time() < end:
            b = usb_status_body(d)
            key = (b.get("cwd"), b["list"].get("version"), b["list"].get("valid")) if b and isinstance(b.get("list"), dict) else None
            same = same + 1 if key is not None and key == seen else 0
            seen = key
            if same >= 3:
                return b
            time.sleep(0.5)
        return None

    cur = first = None
    dirs = count = ver = nfiles = 0
    for attempt in range(3):
        st = stable_status()
        if st is None or st.get("media") != "mounted" or not st["list"].get("valid"):
            record("usb18a: відтворення", False, "потрібен змонтований носій зі стабільним списком", skip=True)
            return
        dirs, count, ver = st["list"]["dirs"], st["list"]["count"], st["list"]["version"]
        nfiles = count - dirs
        if nfiles < 1:
            record("usb18a: відтворення", False, "у поточній папці немає файлів", skip=True)
            return
        status, pg = d.get("/api/usb/list?offset=%d&limit=3&version=%d" % (dirs, ver))
        files = [it for it in pg.get("items", []) if not it.get("dir")] if status == 200 and isinstance(pg, dict) else []
        if not files:
            continue  # список змінився між читаннями (409 stale) — спробуємо ще раз
        first = files[0]
        status, pb = d.post("/api/usb/play", {"index": first["i"], "version": ver})
        if status != 202:
            continue
        cur = wait_until(lambda: (lambda b: b if b and b.get("nowPlaying") and b["nowPlaying"].get("name") == first["name"] and
                                  b["queue"].get("state") == "Playing" else None)(usb_status_body(d)), 8)
        if cur:
            break
    record("usb18a: POST /api/usb/play {index:%s} (перший файл папки) і трек стартував" % (first["i"] if first else "?"),
           bool(cur), "" if cur else "не вдалося запустити перший файл за 3 спроби")
    if not cur:
        return
    cur = usb_settled(d) or cur
    np = cur["nowPlaying"]
    usb18a_shape_checks(cur, "usb18a play")
    record("usb18a: nowPlaying.name == ім'я запущеного файла, dir == cwd",
           np.get("name") == first["name"] and np.get("dir") == cur.get("cwd"),
           "name=%r (список: %r) dir=%r cwd=%r" % (np.get("name"), first["name"], np.get("dir"), cur.get("cwd")))
    q = cur["queue"]
    if not q.get("shuffle"):
        record("usb18a: послідовно -> queue.pos == queue.index + 1", q.get("pos") == q.get("index") + 1,
               "pos=%r index=%r" % (q.get("pos"), q.get("index")))
    record("usb18a: queue.pos <= queue.count", 1 <= q.get("pos", 0) <= q.get("count", 0), "pos=%r count=%r" % (q.get("pos"), q.get("count")))
    # теги: порожні поля = файл без тегів (SKIP), не FAIL
    tagged = bool(np.get("title") or np.get("artist"))
    record("usb18a: тестові файли мають title / artist", tagged,
           "title=%r artist=%r album=%r" % (np.get("title"), np.get("artist"), np.get("album")), skip=not tagged)
    start_path = np["path"]

    # --- seek у межах файла: atEnd == false ---
    dur = np.get("durSec") or 0
    pos0 = np.get("posSec") or 0
    if dur > 0 and pos0 + 2 < dur - 2 and not np.get("paused"):
        ok, b = expect("usb18a: POST /api/usb/seek {deltaSec:2} -> 200", d.post("/api/usb/seek", {"deltaSec": 2}), 200,
                       ["ok", "deltaSec", "atEnd"])
        if ok:
            record("usb18a: seek у межах файла -> atEnd == false", b.get("atEnd") is False, str(b))
    else:
        record("usb18a: seek у межах файла", False, "durSec=%r posSec=%r (замало до кінця або тривалість невідома)" % (dur, pos0), skip=True)

    # --- next: path змінився, теги й pos оновились ---
    prev_pos = q.get("pos")
    if nfiles >= 2:
        expect("usb18a: POST /api/usb/control next", d.post("/api/usb/control", {"action": "next"}), 202, ["reqId"])
        b = wait_track_change(d, start_path, 10)
        record("usb18a: після next nowPlaying.path змінився (<= 10 с)", bool(b), "" if b else "path не змінився")
        if b:
            usb18a_shape_checks(b, "usb18a next")
            if not b["queue"].get("shuffle") and prev_pos:
                record("usb18a: після next queue.pos == %d" % (prev_pos + 1), b["queue"].get("pos") == prev_pos + 1,
                       "pos=%r" % b["queue"].get("pos"))
            cur = b
    else:
        record("usb18a: next / теги оновились", False, "у папці лише один файл", skip=True)

    # --- seek на паузі -> 409 paused ---
    usb_settled(d)
    status, pb = d.post("/api/usb/control", {"action": "pause"})
    if status == 200:
        wait_until(lambda: (usb_status_body(d) or {}).get("queue", {}).get("paused") is True, 6, 0.4)
        expect("usb18a: seek на паузі -> 409 paused", d.post("/api/usb/seek", {"deltaSec": 5}), 409, ["error"], "paused")
        d.post("/api/usb/control", {"action": "resume"})
        wait_until(lambda: (usb_status_body(d) or {}).get("queue", {}).get("paused") is False, 6, 0.4)
    else:
        record("usb18a: seek на паузі -> 409 paused", False, "pause -> HTTP %s" % status, skip=True)

    # --- seek за кінець: atEnd, наступний трек, без read_error ---
    b0 = usb_settled(d) or usb_status_body(d)
    np0 = b0.get("nowPlaying") if b0 else None
    if np0 and (np0.get("durSec") or 0) > 0 and not np0.get("paused"):
        path0, idx0 = np0["path"], b0["queue"].get("index")
        status, sb = d.post("/api/usb/seek", {"deltaSec": 3600})  # kSeekMaxAbsSec: більшого web API не приймає
        record("usb18a: POST /api/usb/seek {deltaSec:3600} -> 200 atEnd:true",
               status == 200 and isinstance(sb, dict) and sb.get("ok") is True and sb.get("atEnd") is True,
               "HTTP %s %s" % (status, str(sb)[:100]))
        if status == 200 and isinstance(sb, dict) and sb.get("atEnd") is True:
            def moved():
                b = usb_status_body(d)
                if not b:
                    return None
                n = b.get("nowPlaying")
                if n and n.get("path") and n["path"] != path0:
                    return b
                if b["queue"].get("state") == "Idle" and b["queue"].get("reason") == "end_of_queue":
                    return b
                return None
            b = wait_until(moved, 10, 0.3)
            record("usb18a: після seek за кінець — наступний трек або end_of_queue (<= 10 с)", bool(b),
                   "" if b else "трек не змінився")
            time.sleep(1.0)
            b = usb_status_body(d) or b
            if b:
                lr = b.get("lastReq") or {}
                reason = b["queue"].get("reason")
                record("usb18a: seek за кінець без read_error / too_many_skips",
                       reason in (None, "end_of_queue") and "read_error" not in str(lr.get("why")),
                       "queue.reason=%r lastReq.why=%r" % (reason, lr.get("why")))
    else:
        record("usb18a: seek за кінець", False, "тривалість треку невідома (durSec == 0) або пауза", skip=True)

    # --- повернення до початкового треку ---
    b = usb_status_body(d)
    lv = b["list"]["version"] if b and isinstance(b.get("list"), dict) else ver
    d.post("/api/usb/play", {"index": first["i"], "version": lv})
    back = wait_until(lambda: (lambda x: x if x and x.get("nowPlaying") and x["nowPlaying"].get("path") == start_path else None)(
        usb_status_body(d)), 10, 0.4)
    record("usb18a: повернуто початковий трек", bool(back), "" if back else "path != %s" % start_path)


def usb_write_checks(d, st, active):
    """[USB15] Негативні POST /api/usb/* без побічних ефектів. 400 перевіряються завжди (валідація йде до перевірки режиму);
    409 usb_mode_off — лише коли режим вимкнений. Коректних команд, що змінюють стан, тут НЕМАЄ (див. hwcheck_USB15.md)."""
    if st and st.get("mode") in ("OtaUpdate", "IrLearn"):
        record("usb POST-перевірки", False, "mode=%s: спершу завершіть OTA/навчання IR" % st.get("mode"), skip=True)
        return
    expect("POST /api/usb/mode {} -> 400", d.post("/api/usb/mode", {}), 400, ["error", "field"], "missing_field")
    expect("POST /api/usb/mode {action:x} -> 400", d.post("/api/usb/mode", {"action": "x"}), 400, ["error", "field"], "invalid_value")
    expect("POST /api/usb/mode {action:1} -> 400", d.post("/api/usb/mode", {"action": 1}), 400, ["error", "field"], "invalid_value")
    expect("POST /api/usb/mode unknown field -> 400", d.post("/api/usb/mode", {"action": "enter", "x": 1}), 400, ["error"],
           "unknown_field")
    expect("POST /api/usb/mode (без тіла) -> 400", d.post("/api/usb/mode"), 400, ["error"], "no_body")
    expect("POST /api/usb/control {} -> 400", d.post("/api/usb/control", {}), 400, ["error", "field"], "missing_field")
    expect("POST /api/usb/control {action:bogus} -> 400", d.post("/api/usb/control", {"action": "bogus"}), 400, ["error", "field"],
           "invalid_value")
    expect("POST /api/usb/seek {} -> 400", d.post("/api/usb/seek", {}), 400, ["error", "field"], "missing_field")
    expect("POST /api/usb/seek {deltaSec:1.5} -> 400", d.post("/api/usb/seek", {"deltaSec": 1.5}), 400, ["error", "field"],
           "invalid_value")
    expect("POST /api/usb/seek {deltaSec:99999} -> 400", d.post("/api/usb/seek", {"deltaSec": 99999}), 400,
           ["error", "field", "min", "max"], "out_of_range")
    expect("POST /api/usb/options {} -> 400", d.post("/api/usb/options", {}), 400, ["error"], "missing_field")
    expect("POST /api/usb/options {shuffle:x} -> 400", d.post("/api/usb/options", {"shuffle": "x"}), 400, ["error", "field"],
           "invalid_value")
    expect("POST /api/usb/options unknown field -> 400", d.post("/api/usb/options", {"shuffle": True, "x": 1}), 400, ["error"],
           "unknown_field")
    expect("POST /api/usb/cd {} -> 400", d.post("/api/usb/cd", {}), 400, ["error", "field"], "missing_field")
    expect("POST /api/usb/cd {up:true,index:1} -> 400", d.post("/api/usb/cd", {"up": True, "index": 1}), 400, ["error", "field"],
           "conflicting_fields")
    expect("POST /api/usb/cd {up:false} -> 400", d.post("/api/usb/cd", {"up": False}), 400, ["error", "field"], "invalid_value")
    expect("POST /api/usb/cd {index:-1} -> 400", d.post("/api/usb/cd", {"index": -1}), 400, ["error", "field", "min", "max"],
           "out_of_range")
    expect("POST /api/usb/cd {index:x} -> 400", d.post("/api/usb/cd", {"index": "x"}), 400, ["error", "field"], "invalid_value")
    expect("POST /api/usb/play {version:1} -> 400", d.post("/api/usb/play", {"version": 1}), 400, ["error", "field"], "missing_field")
    expect("POST /api/usb/play {index:0,up:true} -> 400", d.post("/api/usb/play", {"index": 0, "up": True}), 400, ["error"],
           "unknown_field")
    if active:
        record("usb 409 usb_mode_off", False, "режим «USB» активний: перевірки станів «вимкнено» пропущено", skip=True)
        return
    expect("POST /api/usb/control pause поза режимом -> 409", d.post("/api/usb/control", {"action": "pause"}), 409,
           ["error", "state"], "usb_mode_off")
    expect("POST /api/usb/control next поза режимом -> 409", d.post("/api/usb/control", {"action": "next"}), 409,
           ["error"], "usb_mode_off")
    expect("POST /api/usb/seek поза режимом -> 409", d.post("/api/usb/seek", {"deltaSec": 5}), 409, ["error"], "usb_mode_off")
    expect("POST /api/usb/options поза режимом -> 409", d.post("/api/usb/options", {"shuffle": True}), 409, ["error"],
           "usb_mode_off")
    expect("POST /api/usb/cd поза режимом -> 409", d.post("/api/usb/cd", {"index": 0}), 409, ["error"], "usb_mode_off")
    expect("POST /api/usb/cd up поза режимом -> 409", d.post("/api/usb/cd", {"up": True}), 409, ["error"], "usb_mode_off")
    expect("POST /api/usb/play поза режимом -> 409", d.post("/api/usb/play", {"index": 0}), 409, ["error"], "usb_mode_off")


def player_checks(d, st):
    if not st or st.get("standby") or st.get("input") != 0 or st.get("mode") not in ("Radio", "Menu"):
        record("player.*", False, "потрібен увімкнений пристрій на вході 0 (Radio)", skip=True)
        return
    playing0 = bool(st.get("playing")) or st.get("streamStatus") not in ("Idle",)
    expect("POST /api/player/toggle", d.post("/api/player/toggle"), 200, ["state"])
    expect("POST /api/player/toggle (повернути)", d.post("/api/player/toggle"), 200, ["state"])
    if playing0:
        expect("POST /api/player/play (вже грає, ok)", d.post("/api/player/play"), 200, ["state"])
    else:
        expect("POST /api/player/pause (вже зупинено, ok)", d.post("/api/player/pause"), 200, ["state"])
    if st["station"]["count"] > 1:
        i0 = st["station"]["index"]
        ok, b = expect("POST /api/player/next", d.post("/api/player/next"), 200, ["state"])
        expect("POST /api/player/prev (повернути)", d.post("/api/player/prev"), 200, ["state"])
        if ok:
            record("next змінив stationIndex", b["state"]["stationIndex"] != i0,
                   "%s -> %s" % (i0, b["state"]["stationIndex"]))
    else:
        record("player.next/prev", False, "станція лише одна", skip=True)


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Smoke-перевірка HTTP API пристрою")
    ap.add_argument("host", help="audio.local або IP")
    ap.add_argument("--write", action="store_true", help="перевірки зі зміною стану (з поверненням значень)")
    ap.add_argument("--player", action="store_true", help="(з --write) play/pause та next/prev")
    ap.add_argument("--confirm-checks", action="store_true",
                    help="(з --write) негативні перевірки factory-reset/wifi-reset без confirm")
    ap.add_argument("--timeout", type=float, default=8.0, help="таймаут запиту, с (за замовч. 8)")
    args = ap.parse_args()

    d = Dev(args.host, args.timeout)
    status, _ = d.get("/api/status")
    if status == 0:
        print("Пристрій %s недоступний: %s" % (args.host, _), file=sys.stderr)
        return 2

    st = read_checks(d)
    usb_present, usb_active = usb_read_checks(d)  # [USB15]
    # [INP4b] Узгодженість «веб-активного входу»: у режимі «USB» (usb/status.active) status.input має бути 3; слот 3 доступний
    # (inputs[3].available) тоді й лише тоді, коли USB-маршрути зібрано.
    inp_main = get_inputs(d)
    if usb_present and usb_active:
        status, cur = d.get("/api/status")
        record("режим «USB» активний -> status.input == 3", status == 200 and isinstance(cur, dict) and cur.get("input") == USB_SLOT,
               "got %r" % (cur.get("input") if isinstance(cur, dict) else cur))
    else:
        record("режим «USB» активний -> status.input == 3", False, "режим «USB» не активний (див. hwcheck_INP4b2.md)", skip=True)
    if inp_main is not None:
        record("inputs[3].available == (USB-маршрути присутні)", bool(inp_main["inputs"][USB_SLOT]["available"]) == bool(usb_present),
               "available=%s, usb=%s" % (inp_main["inputs"][USB_SLOT]["available"], usb_present))
    if args.write:
        if usb_present and usb_active:
            # [USB15] У режимі «USB» POST /api/input -> 202 (вихід із режиму), тож старі write-перевірки (очікують 200) й сам режим зламались би.
            record("write-перевірки (звук / станції / входи)", False,
                   "режим USB активний: пропущено; вийдіть з режиму (POST /api/usb/mode exit) і запустіть --write ще раз", skip=True)
        else:
            write_checks(d, st, args)
            if args.player:
                usb_input_checks(d, st)  # [INP4b] чутний вхід у режим «USB» слотом 3 і вихід назад
            else:
                record("режим «USB» через вхід 3 (202 usb_enter, input_active, status.input == 3)", False,
                       "додайте --player", skip=True)
        if usb_present:
            usb_write_checks(d, st, usb_active)
    else:
        record("POST/PUT/DELETE перевірки", False, "додайте --write", skip=True)

    width = max(len(r[1]) for r in results)
    print("\n%-5s  %-*s  %s" % ("", width, "ТЕСТ", "ДЕТАЛІ"))
    print("-" * (width + 20))
    for res, name, detail in results:
        print("%-5s  %-*s  %s" % (res, width, name, detail))
    npass = sum(1 for r in results if r[0] == PASS)
    nfail = sum(1 for r in results if r[0] == FAIL)
    nskip = sum(1 for r in results if r[0] == SKIP)
    print("-" * (width + 20))
    print("PASS %d   FAIL %d   SKIP %d" % (npass, nfail, nskip))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
