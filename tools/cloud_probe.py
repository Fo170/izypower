#!/usr/bin/env python3
"""Sonde de l'API cloud Izypower (application.izypowercloud.fr).

Valide le flux complet login -> stations -> station info -> device page -> component (PV)
et dump les réponses JSON brutes dans /tmp/opencode/izypower/dump_cloud/ pour analyse.

Usage:
    python3 cloud_probe.py <email> <mot_de_passe>
"""
import json
import sys
import urllib.parse
import urllib.request
from pathlib import Path
from datetime import datetime

BASE = "http://application.izypowercloud.fr/photo_voltaic/api"
OUT = Path("/tmp/opencode/izypower/dump_cloud")
OUT.mkdir(parents=True, exist_ok=True)


def http(method, url, headers=None, body=None, timeout=20):
    req = urllib.request.Request(url, method=method, headers=headers or {})
    if body is not None:
        req.data = json.dumps(body).encode()
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
            try:
                return r.status, json.loads(raw)
            except Exception:
                return r.status, raw.decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def main(username, password):
    base_headers = {
        "app-platform": "izy",
        "User-Agent": "Mozilla/5.0 (Linux; Android 16; Pixel 10a)",
        "Accept-Language": "fr-FR,fr;q=0.9,en;q=0.5",
    }

    print(f"== login == {BASE}/login")
    status, data = http("POST", f"{BASE}/login", base_headers, {"username": username, "password": password})
    print(f"status={status}")
    (OUT / "01_login.json").write_text(json.dumps(data, indent=2, ensure_ascii=False))
    if status != 200:
        print("ECHEC login:", data)
        return 1
    token = (data.get("data") or {}).get("token") or data.get("token")
    if not token:
        print("Token introuvable dans:", json.dumps(data, ensure_ascii=False)[:500])
        return 1
    print(f"token OK ({len(token)} octets), exp JWT: {token.split('.')[1][:40]}...")
    headers = dict(base_headers)
    headers["x-tts-access-token"] = token

    print("\n== stations ==")
    status, data = http("GET", f"{BASE}/powerStations/page?page=1&limit=100", headers)
    print(f"status={status}")
    (OUT / "02_stations.json").write_text(json.dumps(data, indent=2, ensure_ascii=False))
    records = (data.get("data") or {}).get("records") or []
    print(f"{len(records)} station(s)")
    if not records:
        print("Aucune station -> fin")
        return 0
    for rec in records:
        print("  -", rec.get("stationsId"), rec.get("stationName"), "cap=", rec.get("installedCapacity"))

    today = datetime.now().strftime("%Y-%m-%d")
    for rec in records:
        sid = rec.get("stationsId")
        print(f"\n== station {sid} info ==")
        status, info = http("GET", f"{BASE}/v3/powerStations/info/{sid}", headers)
        print(f"status={status}")
        (OUT / f"03_station_{sid}_info.json").write_text(json.dumps(info, indent=2, ensure_ascii=False))
        if status == 200:
            inf = (info.get("data") or {})
            extra = inf.get("extraData") or {}
            print("  power(W)=", extra.get("power"), "grid=", extra.get("grid_power"),
                  "cons=", extra.get("consumption"), "batt=", extra.get("battery_power"),
                  "soc=", extra.get("battery_soc"), "lastUpdate=", inf.get("lastUpdate"))
            prod = extra.get("production") or {}
            print("  prod energy:", {k: prod.get(k) for k in ("day", "month", "year", "all")})

        print(f"\n== station {sid} device/page ==")
        q = urllib.parse.quote(f"{BASE}/device/page?powerId={sid}&deviceType=all&page=1&limit=100", safe=":/?&=")
        status, devices = http("GET", q, headers)
        print(f"status={status}")
        (OUT / f"04_station_{sid}_devices.json").write_text(json.dumps(devices, indent=2, ensure_ascii=False))
        if status == 200:
            drecs = ((devices.get("data") or {}).get("records")) or []
            print(f"  {len(drecs)} onduleur(s)")
            for d in drecs:
                print("   -", d.get("deviceId"), d.get("deviceName"), "| type=", d.get("deviceType"),
                      "| sn=", d.get("sn") or d.get("serialNumber"), "| online=", d.get("onlineState"),
                      "| sw=", d.get("softwareVersion"))
                dtos = d.get("dataDtos") or []
                for dto in dtos:
                    print(f"       dataDto {dto.get('key')} = {dto.get('value')}")

        print(f"\n== station {sid} component (PV) ==")
        status, comp = http("GET", f"{BASE}/component/{sid}?searchTime={today}", headers)
        print(f"status={status}")
        (OUT / f"05_station_{sid}_component.json").write_text(json.dumps(comp, indent=2, ensure_ascii=False))
        if status == 200:
            pvs = comp.get("pvData") or []
            print(f"  {len(pvs)} chaîne(s) PV")
            for pv in pvs:
                print("   - sn=", pv.get("sn"), "| pv=", pv.get("pv"), "| pvPower=", pv.get("pvPower"))

        for d in (devices.get("data") or {}).get("records") or []:
            sn = d.get("sn") or d.get("serialNumber")
            if sn:
                print(f"\n== device wifi {sn} ==")
                status, wifi = http("GET", f"{BASE}/v3/device/wifi/{sn}", headers)
                print(f"status={status}")
                (OUT / f"06_device_{sn}_wifi.json").write_text(json.dumps(wifi, indent=2, ensure_ascii=False))
                if status == 200:
                    print("  wifi:", json.dumps(wifi.get("data") or wifi, ensure_ascii=False)[:300])

    print("\nDumps JSON dans:", OUT)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    sys.exit(main(sys.argv[1], sys.argv[2]))
