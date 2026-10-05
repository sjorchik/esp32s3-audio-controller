#!/usr/bin/env python3
"""Smoke-перевірка HTTP API пристрою (ESP32-S3 Audio Controller). Лише stdlib Python 3.

Запуск:
    python tools/web_api_smoke.py audio.local             # тільки безпечні GET
    python tools/web_api_smoke.py audio.local --write     # + зміна й ПОВЕРНЕННЯ значень
    python tools/web_api_smoke.py 192.168.1.50 --write --player

Режими:
    (без прапорців)   лише GET. Нічого не змінює на пристрої.
    --write           POST/PUT/DELETE-перевірки: гучність, мʼют, gain, яскравість, тембр, вхід (без зміни),
                      станція-тимчасова (додається й видаляється), негативні випадки (очікувані 400).
                      Кожна зміна повертається до початкового значення.
    --player          (разом з --write) перемикання play/pause та next/prev станції: чутні переходи.
    --confirm-checks  (разом з --write) негативні перевірки factory-reset / wifi-reset БЕЗ підтвердження
                      (очікується 400 confirm_required). Якщо прошивка має баг і не перевіряє confirm,
                      пристрій скине налаштування/Wi-Fi й перезапуститься — тому окремий прапорець.

НІКОЛИ не запускається: POST /api/system/reboot, /api/ota, /api/stations/import, /api/ir/map/import,
/api/power (standby), реальні factory-reset / wifi-reset.

Код виходу: 0 — немає FAIL; 1 — є хоча б один FAIL; 2 — пристрій недоступний.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"
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
                    ["mode", "standby", "input", "inputName", "inputs", "volume", "mute", "playing",
                     "streamStatus", "station", "track", "wifi", "processor", "otaProgress",
                     "heap", "uptimeMs"])
    if ok:
        record("status.station.{index,count,max}", has_keys(st["station"], ["index", "count", "max", "name"]))
        record("status.inputs[] має index/name/available",
               isinstance(st["inputs"], list) and len(st["inputs"]) > 0 and
               all(has_keys(i, ["index", "name", "available"]) for i in st["inputs"]))
        proc = st["processor"]
        record("status.processor.{type,ready,capabilities}", has_keys(proc, ["type", "ready", "capabilities"]))
    expect("GET /api/settings", d.get("/api/settings"), 200,
           ["processorType", "inputNames", "brightness", "displayFlipped", "bass", "treble",
            "balance", "loudness", "lastInput", "lastStation", "lastVolume", "lastMute"])

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
        record("ir/actions не містить OK/BACK", "OK" not in names and "BACK" not in names)

    status, mp = d.get("/api/ir/map")
    record("GET /api/ir/map", status == 200 and isinstance(mp, list), "HTTP %s" % status)
    expect("GET /api/ir/learn/status", d.get("/api/ir/learn/status"), 200,
           ["status", "target", "conflictWith"])
    expect("GET /api/system", d.get("/api/system"), 200,
           ["firmware", "chip", "uptimeMs", "resetReason", "heap", "psram", "fs", "ota", "wifi", "settings"])
    expect("GET /api/nonexistent -> 404", d.get("/api/nonexistent"), 404, ["error"], "not_found")
    return st if ok else None


# --------------------------------------------------------------------------
# Перевірки зі зміною стану (--write), кожна повертає початкове значення
# --------------------------------------------------------------------------
def write_checks(d, st, args):
    caps = (st.get("processor") or {}).get("capabilities") if st else None
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
    expect("POST /api/ir/learn OK (not learnable) -> 400", d.post("/api/ir/learn", {"action": "OK"}), 400, ["error"])
    status, _ = d.post("/api/ota")
    record("POST /api/ota без тіла -> 4xx (не стартує)", 400 <= status < 500, "HTTP %s" % status)

    if caps is None:
        record("аудіо-перевірки", False, "processor.ready=false (немає аудіопроцесора)", skip=True)
    else:
        vmin, vmax = caps["volumeMin"], caps["volumeMax"]
        expect("POST /api/volume above max -> 400", d.post("/api/volume", {"value": vmax + 1}), 400, ["error", "min", "max"],
               "out_of_range")
        bad_input = caps["inputCount"]
        expect("POST /api/input out of range -> 400", d.post("/api/input", {"index": bad_input}), 400,
               ["error", "min", "max"], "out_of_range")
        expect("POST /api/input {index:cur} (без зміни)", d.post("/api/input", {"index": st["input"]})
               if not standby else (409, {"error": "standby"}), 409 if standby else 200)

        if standby:
            record("volume/mute/gain/tone зміни", False, "пристрій у standby (очікувано 409)", skip=True)
            expect("POST /api/volume у standby -> 409", d.post("/api/volume", {"value": st["volume"]}), 409, ["error"], "standby")
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

            # --- gain ---
            if caps.get("inputGain") and caps["gainMax"] > caps["gainMin"]:
                g0 = st["gain"]
                g1 = g0 + 1 if g0 < caps["gainMax"] else g0 - 1
                expect("POST /api/gain {value}", d.post("/api/gain", {"value": g1}), 200, ["state"])
                status, now = d.get("/api/status")
                record("gain: /api/status.gain == %d" % g1, status == 200 and now.get("gain") == g1,
                       "got %s" % (now.get("gain") if isinstance(now, dict) else now))
                expect("POST /api/gain повернути %d" % g0, d.post("/api/gain", {"value": g0}), 200, ["state"])
                expect("POST /api/gain out of range -> 400", d.post("/api/gain", {"value": caps["gainMax"] + 1}), 400,
                       ["error"], "out_of_range")
            else:
                record("gain", False, "чип не підтримує gain", skip=True)

            # --- тембр через /api/settings ---
            if caps.get("bass"):
                b0 = st["bass"]
                b1 = b0 + 1 if b0 < caps["toneMax"] else b0 - 1
                expect("POST /api/settings {bass}", d.post("/api/settings", {"bass": b1}), 200, ["ok", "results"])
                expect("POST /api/settings повернути bass", d.post("/api/settings", {"bass": b0}), 200, ["ok"])
            else:
                record("bass через /api/settings", False, "чип не підтримує bass", skip=True)

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
    if args.write:
        write_checks(d, st, args)
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
