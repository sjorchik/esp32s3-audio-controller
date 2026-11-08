#!/usr/bin/env python3
"""Smoke-перевірка HTTP API пристрою (ESP32-S3 Audio Controller). Лише stdlib Python 3.

Запуск:
    python tools/web_api_smoke.py audio.local             # тільки безпечні GET
    python tools/web_api_smoke.py audio.local --write     # + зміна й ПОВЕРНЕННЯ значень
    python tools/web_api_smoke.py 192.168.1.50 --write --player

Режими:
    (без прапорців)   лише GET. Нічого не змінює на пристрої.
    --write           POST/PUT/DELETE-перевірки: гучність, мʼют, баланс і чутливість VU (активний і неактивний вхід), яскравість,
                      вхід (без зміни), станція-тимчасова (додається й видаляється), негативні випадки
                      (очікувані 400/404: застарілі ключі /api/settings, /api/gain тощо).
                      Кожна зміна повертається до початкового значення.
    --player          (разом з --write) перемикання play/pause та next/prev станції: чутні переходи.
    --confirm-checks  (разом з --write) негативні перевірки factory-reset / wifi-reset БЕЗ підтвердження
                      (очікується 400 confirm_required). Якщо прошивка має баг і не перевіряє confirm,
                      пристрій скине налаштування/Wi-Fi й перезапуститься — тому окремий прапорець.

НІКОЛИ не запускається: POST /api/system/reboot, /api/ota, /api/stations/import, /api/ir/map/import,
/api/power (standby), реальні factory-reset / wifi-reset.

Оновлено в P50a: без processor / gain / тембру; додано GET /api/balance, menuContext, 3 входи.
Оновлено у V2: GET /api/vu, status.vuSens; з --write — чутливість VU активного й неактивного входу (зміна й повернення),
негативні випадки POST /api/vu (value поза діапазоном, невідомий ключ, input = 3).

Оновлено в USB15: GET /api/usb/status|list|result (форма відповідей; за режиму «USB» вимкненому — 409 usb_mode_off на list); з --write — негативні
перевірки POST /api/usb/* (400 за тілом; 409 usb_mode_off, лише коли режим вимкнений). Нічого не змінює: POST /api/usb/mode з коректною дією
(вхід у режим) НЕ викликається — це перевіряється вручну за hwcheck_USB15.md. Якщо /api/usb/status повертає 404 (збірка з
FEATURE_USB_MODE = 0), усі USB-перевірки позначаються SKIP.

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
INPUT_COUNT = 3                          # defaults::kInputCount (0 радіо, 1 TV Box, 2 Computer)
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
        record("status.inputs[] має index/name/available, рівно %d" % INPUT_COUNT,
               isinstance(st["inputs"], list) and len(st["inputs"]) == INPUT_COUNT and
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
        record("settings.inputNames рівно %d" % INPUT_COUNT,
               isinstance(sett["inputNames"], list) and len(sett["inputNames"]) == INPUT_COUNT)

    # --- GET /api/balance (P50a) ---
    ok_b, bal = expect("GET /api/balance (активний вхід)", d.get("/api/balance"), 200, ["input", "balance"])
    if ok_b and ok:
        record("balance: input == status.input", bal["input"] == st["input"], "%s vs %s" % (bal["input"], st["input"]))
        record("balance: balance == status.balance", bal["balance"] == st["balance"],
               "%s vs %s" % (bal["balance"], st["balance"]))
    for n in range(INPUT_COUNT):
        okn, bn = expect("GET /api/balance?input=%d" % n, d.get("/api/balance?input=%d" % n), 200, ["input", "balance"])
        if okn:
            record("balance?input=%d: input == %d, значення в межах" % (n, n),
                   bn["input"] == n and isinstance(bn["balance"], int) and BALANCE_MIN <= bn["balance"] <= BALANCE_MAX,
                   str(bn))
    status, body = d.get("/api/balance?input=%d" % INPUT_COUNT)
    record("GET /api/balance?input=%d -> 400 out_of_range" % INPUT_COUNT,
           status == 400 and isinstance(body, dict) and body.get("error") == "out_of_range" and
           body.get("field") == "input" and body.get("min") == 0 and body.get("max") == INPUT_COUNT - 1,
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
        good = (isinstance(vins, list) and len(vins) == INPUT_COUNT and
                all(has_keys(i, ["index", "sensitivityDb"]) for i in vins) and
                [i["index"] for i in vins] == list(range(INPUT_COUNT)))
        record("vu.inputs[] рівно %d, index 0..%d, має sensitivityDb" % (INPUT_COUNT, INPUT_COUNT - 1), good, str(vins)[:100])
        if good:
            record("vu: sensitivityDb цілі в %d..%d" % (VU_MIN, VU_MAX),
                   all(isinstance(i["sensitivityDb"], int) and not isinstance(i["sensitivityDb"], bool) and
                       VU_MIN <= i["sensitivityDb"] <= VU_MAX for i in vins), str(vins)[:100])
        if ok:
            record("vu: input == status.input", vu["input"] == st["input"], "%s vs %s" % (vu["input"], st["input"]))
            if good and not st["standby"] and 0 <= st["input"] < INPUT_COUNT:
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
    status, resp = d.post("/api/settings", {"inputNames": ["a", "b"]})
    codes = [e.get("code") for e in resp.get("errors", [])] if isinstance(resp, dict) else []
    record("POST /api/settings inputNames (2 елементи) -> 400 expected_array_of_3",
           status == 400 and codes == ["expected_array_of_3"], "HTTP %s %s" % (status, str(resp)[:80]))
    expect("POST /api/balance {} -> 400 missing_field", d.post("/api/balance", {}), 400, ["error"], "missing_field")
    expect("POST /api/balance unknown field -> 400", d.post("/api/balance", {"value": 0, "x": 1}), 400, ["error"],
           "unknown_field")
    expect("POST /api/balance value=%d -> 400 out_of_range" % (BALANCE_MAX + 1),
           d.post("/api/balance", {"value": BALANCE_MAX + 1}), 400, ["error", "min", "max"], "out_of_range")
    expect("POST /api/balance input=%d -> 400 out_of_range" % INPUT_COUNT,
           d.post("/api/balance", {"value": 0, "input": INPUT_COUNT}), 400, ["error", "min", "max"], "out_of_range")

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
    expect("POST /api/vu input=%d -> 400 out_of_range" % INPUT_COUNT,
           d.post("/api/vu", {"value": 0, "input": INPUT_COUNT}), 400, ["error", "min", "max"], "out_of_range")
    status, body = d.post("/api/vu", {"value": VU_MAX + 1})
    record("POST /api/vu out_of_range: field=value, min/max == %d/%d" % (VU_MIN, VU_MAX),
           status == 400 and isinstance(body, dict) and body.get("field") == "value" and
           body.get("min") == VU_MIN and body.get("max") == VU_MAX, "HTTP %s %s" % (status, str(body)[:80]))

    vmin, vmax = VOLUME_MIN, VOLUME_MAX
    expect("POST /api/volume above max -> 400", d.post("/api/volume", {"value": vmax + 1}), 400, ["error", "min", "max"],
           "out_of_range")
    bad_input = INPUT_COUNT
    expect("POST /api/input out of range -> 400", d.post("/api/input", {"index": bad_input}), 400,
           ["error", "min", "max"], "out_of_range")
    expect("POST /api/input {index:cur} (без зміни)", d.post("/api/input", {"index": st["input"]})
           if not standby else (409, {"error": "standby"}), 409 if standby else 200)

    # --- баланс неактивного входу: дозволено й у standby (лише профіль у Settings) ---
    other = (st["input"] + 1) % INPUT_COUNT
    status, o0 = d.get("/api/balance?input=%d" % other)
    if status == 200 and isinstance(o0, dict):
        b0 = o0["balance"]
        b1 = b0 + 1 if b0 < BALANCE_MAX else b0 - 1
        ok, _ = expect("POST /api/balance {value, input:%d} (неактивний)" % other,
                       d.post("/api/balance", {"value": b1, "input": other}), 200, ["input", "balance"])
        if ok:
            status, now = d.get("/api/balance?input=%d" % other)
            record("balance (неактивний): GET == %d" % b1, status == 200 and now.get("balance") == b1, str(now))
        expect("POST /api/balance повернути %d (вхід %d)" % (b0, other),
               d.post("/api/balance", {"value": b0, "input": other}), 200, ["input", "balance"])
    else:
        record("balance неактивного входу", False, "не вдалося прочитати", skip=True)

    # --- V2: чутливість VU неактивного входу: дозволено й у standby (лише профіль у Settings) ---
    status, w0 = d.get("/api/vu")
    if status == 200 and isinstance(w0, dict) and isinstance(w0.get("inputs"), list) and len(w0["inputs"]) == INPUT_COUNT:
        vu_act = w0["input"]
        vu_other = (vu_act + 1) % INPUT_COUNT
        u0 = w0["inputs"][vu_other]["sensitivityDb"]
        u1 = u0 + 1 if u0 < VU_MAX else u0 - 1
        ok, b = expect("POST /api/vu {value, input:%d} (неактивний)" % vu_other,
                       d.post("/api/vu", {"value": u1, "input": vu_other}), 200, ["input", "sensitivityDb"])
        if ok:
            record("vu (неактивний): відповідь input == %d, sensitivityDb == %d" % (vu_other, u1),
                   b["input"] == vu_other and b["sensitivityDb"] == u1, str(b))
            status, now = d.get("/api/vu")
            record("vu (неактивний): GET inputs[%d] == %d" % (vu_other, u1),
                   status == 200 and now["inputs"][vu_other]["sensitivityDb"] == u1, str(now)[:100])
            if status == 200:
                others_ok = all(now["inputs"][i]["sensitivityDb"] == w0["inputs"][i]["sensitivityDb"]
                                for i in range(INPUT_COUNT) if i != vu_other)
                record("vu (неактивний): інші входи не змінились", others_ok, str(now["inputs"])[:100])
        expect("POST /api/vu повернути %d (вхід %d)" % (u0, vu_other),
               d.post("/api/vu", {"value": u0, "input": vu_other}), 200, ["input", "sensitivityDb"])
    else:
        record("vu неактивного входу", False, "не вдалося прочитати", skip=True)

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
    if args.write:
        if usb_present and usb_active:
            # [USB15] У режимі «USB» POST /api/input -> 202 (вихід із режиму), тож старі write-перевірки (очікують 200) й сам режим зламались би.
            record("write-перевірки (звук / станції / входи)", False,
                   "режим USB активний: пропущено; вийдіть з режиму (POST /api/usb/mode exit) і запустіть --write ще раз", skip=True)
        else:
            write_checks(d, st, args)
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
