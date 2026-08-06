#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Validation hors-ligne du client Solarman V5 (portage exact de
IzypowerSolarman.h). Construit une requête V5, génère une réponse de
logger avec des registres connus, puis vérifie le décodage 32 bits,
signé, température, S/N, etc."""

def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else crc >> 1
    return crc & 0xFFFF

SERIAL = 0x0A0B0C0D
SER_B = bytes([SERIAL & 0xFF, (SERIAL>>8)&0xFF, (SERIAL>>16)&0xFF, (SERIAL>>24)&0xFF])

def build_request(addr, qty, seq):
    mb = bytes([0x01, 0x03, (addr>>8)&0xFF, addr&0xFF, (qty>>8)&0xFF, qty&0xFF])
    crc = crc16_modbus(mb)
    mb += bytes([crc & 0xFF, (crc>>8)&0xFF])
    length = 15 + len(mb)
    f = bytearray([0xA5, length & 0xFF, (length>>8)&0xFF, 0x10, 0x45, seq & 0xFF, 0x00])
    f += SER_B
    f += bytes([0x02, 0x00, 0x00, 0,0,0,0, 0,0,0,0, 0,0,0,0])
    f += mb
    f.append(sum(f[1:]) & 0xFF)
    f.append(0x15)
    return bytes(f)

def build_response(regs, seq, double_crc=False):
    """regs: dict addr -> valeur. Payload réponse = 14 octets (frametype,
    status, 3×4 octets) puis Modbus RTU ; Modbus à l'index 25."""
    mb_data = bytearray()
    for a in range(min(regs), max(regs)+1):
        v = regs[a]
        mb_data += bytes([(v>>8)&0xFF, v&0xFF])
    mb = bytes([0x01, 0x03, len(mb_data)]) + bytes(mb_data)
    crc = crc16_modbus(mb)
    mb += bytes([crc & 0xFF, (crc>>8)&0xFF])
    if double_crc:
        mb += bytes([0x00, 0x00])          # bug DEYE : double CRC
    length = 14 + len(mb)                  # payload réponse = 14 + modbus
    r = bytearray([0xA5, length & 0xFF, (length>>8)&0xFF, 0x10, 0x15, seq & 0xFF, 0x00])
    r += SER_B
    r += bytes([0x02, 0x01, 0,0,0,0, 0,0,0,0, 0,0,0,0])  # frametype, status, 3×4
    r += mb
    r.append(sum(r[1:]) & 0xFF)
    r.append(0x15)
    return bytes(r)

def parse_response(frame, seq, strip_double_crc=True):
    """Portage de _readFrame + validation + extraction modbus."""
    assert frame[0] == 0xA5 and frame[-1] == 0x15
    assert frame[3] == 0x10
    assert frame[4] == 0x15
    assert frame[5] == seq
    assert frame[7:11] == SER_B
    assert frame[11] == 0x02
    length = frame[1] | (frame[2] << 8)
    assert len(frame) == 13 + length
    assert sum(frame[1:-2]) & 0xFF == frame[-2]
    mb = frame[25:-2]                      # réponse : modbus à l'index 25
    if strip_double_crc and mb[-2:] == b"\x00\x00" and len(mb) >= 7:
        if crc16_modbus(mb[:-4]) == ((mb[-4] | (mb[-3] << 8)) & 0xFFFF):
            mb = mb[:-2]
    assert mb[1] == 0x03
    assert crc16_modbus(mb) == 0
    out = {}
    for i in range(mb[2] // 2):
        out[i] = (mb[3 + 2*i] << 8) | mb[4 + 2*i]
    return out

if __name__ == "__main__":
    # 1) requête construite comme dans le firmware
    req = build_request(0x0001, 0x007D, seq=0x2F)
    print(f"Requête ({len(req)} octets) : {req.hex(' ')}")

    # 2) registres "réels" simulés
    regs = {}
    for a in range(0x0001, 0x007E):
        regs[a] = 0x1111  # valeur sentinelle
    regs[0x003B] = 2                     # status Normal
    regs[0x003C] = 123                   # today = 12,3 kWh
    regs[0x003F] = 0x1A2B; regs[0x0040] = 0x0001  # total = 0x00011A2B = 72235 -> 7223,5 kWh
    regs[0x0049] = 2301                  # 230,1 V
    regs[0x004C] = 0xFFFF - 25 + 1       # -2,5 A  (signé)
    regs[0x004F] = 5001                  # 50,01 Hz
    regs[0x0056] = 0x4850; regs[0x0057] = 0x0000  # power = 0x4850 = 18512 -> 1851,2 W
    regs[0x005A] = 4200                  # temp = (4200-1000)*0.01 = 32 °C
    regs[0x006D] = 362; regs[0x006E] = 75   # PV1 36,2 V / 7,5 A -> 271,5 W
    regs[0x006F] = 350; regs[0x0070] = 50   # PV2 35,0 V / 5,0 A -> 175 W
    regs[0x0003] = ord('1')<<8 | ord('2')   # S/N "1234567890"
    regs[0x0004] = ord('3')<<8 | ord('4')
    regs[0x0005] = ord('5')<<8 | ord('6')
    regs[0x0006] = ord('7')<<8 | ord('8')
    regs[0x0007] = ord('9')<<8 | ord('0')
    regs[0x0010] = 10000                  # rated power 1000 W

    resp = build_response(regs, seq=0x2F)
    got = parse_response(resp, seq=0x2F)

    # 3) même réponse avec double-CRC (bug DEYE)
    resp2 = build_response(regs, seq=0x2F, double_crc=True)
    got2 = parse_response(resp2, seq=0x2F)
    assert got == got2, "double CRC mal géré"

    def R(a):
        return got[a - 0x0001]

    s16 = lambda v: v - 0x10000 if v & 0x8000 else v
    u32 = lambda lo, hi: lo | (hi << 16)

    serial = ''.join(chr((R(a) >> 8) & 0xFF) + chr(R(a) & 0xFF) for a in range(0x0003, 0x0008))
    checks = {
        "serial": serial == "1234567890",
        "status": R(0x003B) == 2,
        "today": abs(R(0x003C) * 0.1 - 12.3) < 1e-6,
        "total": abs(u32(R(0x003F), R(0x0040)) * 0.1 - 7223.5) < 1e-6,
        "gridV": abs(R(0x0049) * 0.1 - 230.1) < 1e-6,
        "gridI": abs(s16(R(0x004C)) * 0.1 - (-2.5)) < 1e-6,
        "freq": abs(R(0x004F) * 0.01 - 50.01) < 1e-6,
        "power": abs(u32(R(0x0056), R(0x0057)) * 0.1 - 1851.2) < 1e-6,
        "temp": abs((R(0x005A) - 1000) * 0.01 - 32.0) < 1e-6,
        "pv1": abs(R(0x006D) * 0.1 * (R(0x006E) * 0.1) - 271.5) < 1e-6,
        "pv2": abs(R(0x006F) * 0.1 * (R(0x0070) * 0.1) - 175.0) < 1e-6,
        "rated": R(0x0010) * 0.1 == 1000.0,
    }
    ok = True
    for k, v in checks.items():
        print(f"  {'OK ' if v else 'KO '} {k}")
        ok = ok and v
    print("=>", "TOUS LES TESTS PASSENT" if ok else "ÉCHEC")
    raise SystemExit(0 if ok else 1)
