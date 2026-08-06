#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_local.py — Diagnostic de l'accès LOCAL aux micro-onduleurs IZYPOWER
iP-1000-W2 / iP-2000-W4 (Materfrance).

Ce script détermine quel chemin utiliser pour lire les données :
  1. Scan des ports TCP courants (8080 INDEVOLT/RPC, 8899 Solarman V5/Modbus,
     502 Modbus, 80 web, …).
  2. Découverte UDP Solarman (trame "WIFIKIT-214028-READ" → port 48899),
     comme fait par pysolarmanv5/solarman_scan.
  3. Si pysolarmanv5 est installé et qu'un port 8899 répond : tentative de
     lecture réelle des registres (informations du logger).

Usage :
    python3 tools/test_local.py [--pysolarman]

Résultats possibles :
    - port 8899 OUVERT + réponse UDP 48899  → voie locale SOLARMAN V5 (temps réel)
    - port 8080 OUVERT + réponse Indevolt   → voie locale INDEVOLT RPC (comme Titan)
    - rien d'ouvert                          → seul le cloud Izypower est utilisable
"""

import socket
import sys

HOSTS = ["192.168.1.100", "192.168.1.101"]
TCP_PORTS = [80, 443, 502, 8080, 8899, 9090, 10000, 48899, 57000]
SOLARMAN_BROADCAST = ("255.255.255.255", 48899)
SOLARMAN_READ = b"WIFIKIT-214028-READ"


def scan_tcp(host: str, port: int, timeout: float = 1.5) -> bool:
    """Retourne True si le port TCP est joignable."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def discover_solarman(timeout: float = 3.0):
    """Émet la trame de découverte Solarman et collecte les réponses."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)
    found = []
    try:
        sock.sendto(SOLARMAN_READ, SOLARMAN_BROADCAST)
        while True:
            try:
                data, addr = sock.recvfrom(1024)
            except socket.timeout:
                break
            try:
                vals = data.decode().split(",")
                found.append({"host": addr[0], "mac": vals[1], "serial": vals[2]})
            except Exception:
                found.append({"host": addr[0], "raw": repr(data)})
    finally:
        sock.close()
    return found


def try_solarman_v5(host: str, serial: str):
    """Tente de lire des registres via pysolarmanv5 (si installé)."""
    try:
        from pysolarmanv5 import PySolarmanV5
    except ImportError:
        print("    (pysolarmanv5 non installé — `pip install pysolarmanv5`)")
        return
    try:
        p = PySolarmanV5(host, int(serial), port=8899, mb_slave_id=1)
        data = p.read_input_registers(0, 30, 1)
        print(f"    Lecture registres OK: {data}")
    except Exception as exc:
        print(f"    Lecture Solarman V5 impossible: {exc}")


def main():
    args = sys.argv[1:]
    use_pysolarman = "--pysolarman" in args
    serial = None
    if "--serial" in args:
        serial = args[args.index("--serial") + 1]

    for host in HOSTS:
        print(f"--- {host} ---")
        for port in TCP_PORTS:
            if scan_tcp(host, port):
                print(f"  [TCP {port:>5}] OUVERT")
        print("  (scan TCP terminé)")

    print("\n--- Découverte UDP Solarman (48899) ---")
    found = discover_solarman()
    if found:
        for dev in found:
            print(f"  {dev}")
    else:
        print("  Aucune réponse — module WiFi non compatible Solarman V5 "
              "(ou broadcast bloqué).")

    print("\n--- Test de lecture Solarman V5 (pysolarmanv5) ---")
    if use_pysolarman:
        targets = found if found else [{"host": h, "serial": serial} for h in HOSTS]
        for dev in targets:
            sn = dev.get("serial") or serial
            if not sn:
                print(f"  {dev['host']} : pas de S/N logger — fournir --serial <SN>")
                continue
            try_solarman_v5(dev["host"], sn)
    else:
        print("  (installer pysolarmanv5 et ajouter --pysolarman pour tester)")

    print("\nConclusion :")
    print("  - port 8899 OUVERT + réponse 48899 → voie locale SOLARMAN V5 (temps réel)")
    print("  - port 8080 OUVERT                  → voie locale INDEVOLT RPC (comme Titan)")
    print("  - rien                              → seul le CLOUD Izypower (3 min)")


if __name__ == "__main__":
    main()
