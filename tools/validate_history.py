#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
validate_history.py — valide HORS LIGNE la partie « historique » de IzypowerCloud.h

Portage exact en Python de :
  - _epochFromDateTime()      : "2026", "2026-11", "2026-11-04", "2026-11-04 14:30[:00]"
  - _normalizeDate()          : format du searchTime imposé par la granularité
  - _histField()/_histValue() : lecture `time` puis `val`/`value`
  - _histAppend()/_histClose(): décimation adaptative + conservation du dernier point
  - valueAt()                 : interpolation linéaire (bornes, hors plage, cas dégénérés)

Aucune dépendance, aucun réseau, aucun identifiant.

    python3 tools/validate_history.py     # → « TOUS LES TESTS PASSENT »
"""

import sys

IZY_MAX_POINTS = 96          # IZY_MAX_POINTS (défaut de la lib)
IZY_MAX_SERIES = 8           # IZY_MAX_SERIES (défaut de la lib)

RANGE_DAY, RANGE_MONTH, RANGE_YEAR, RANGE_ALL = 0, 1, 2, 3


# ---------------------------------------------------------------- calendrier
def days_from_civil(y, m, d):
    """Howard Hinnant — identique à _daysFromCivil() du header (epoch 1970-01-01)."""
    y -= m <= 2
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def epoch_from_datetime(s):
    """_epochFromDateTime() : 0 si inexploitable, granularités partielles gérées."""
    if not s:
        return 0
    txt = s.strip().replace("T", " ")
    date_part, _, time_part = txt.partition(" ")
    nums = [int(x) for x in date_part.split("-") if x.strip().isdigit()]
    if not nums:
        return 0
    y = nums[0]
    m = nums[1] if len(nums) > 1 else 1
    d = nums[2] if len(nums) > 2 else 1
    h = mi = sec = 0
    if time_part:
        hms = [int(x) for x in time_part.split(":") if x.strip().isdigit()]
        if len(hms) > 0:
            h = hms[0]
        if len(hms) > 1:
            mi = hms[1]
        if len(hms) > 2:
            sec = hms[2]
    if not (1970 <= y <= 2200 and 1 <= m <= 12 and 1 <= d <= 31):
        return 0
    days = days_from_civil(y, m, d)
    if days < 0:
        return 0
    return days * 86400 + h * 3600 + mi * 60 + sec


# ------------------------------------------------------------------ requête
def normalize_date(rng, s):
    """_normalizeDate() : day→10, month→7, year→4, all→inchangé."""
    want = {RANGE_MONTH: 7, RANGE_YEAR: 4}.get(rng, 10)
    return (s or "")[:want]


def range_name(rng):
    return {RANGE_MONTH: "month", RANGE_YEAR: "year", RANGE_ALL: "all"}.get(rng, "day")


# ------------------------------------------------------------------- séries
class Series:
    def __init__(self):
        self.key = ""
        self.name = ""
        self.unit = ""
        self.line_type = "line"
        self.points = []          # [(epoch, value)]
        self.point_total = 0


class HistCtx:
    def __init__(self):
        self.slot = 0             # série en cours d'accumulation
        self.seen = 0
        self.stride = 1
        self.pending_ep = 0
        self.last_ep = 0
        self.last_val = 0.0
        self.have_pending = False
        self.have_last = False


def hist_append(series, ctx, ep, val):
    """_histAppend() : remplit jusqu'à IZY_MAX_POINTS puis divise par deux."""
    ctx.last_ep, ctx.last_val, ctx.have_last = ep, val, True
    if ctx.seen % ctx.stride != 0:
        ctx.seen += 1
        return
    if len(series.points) < IZY_MAX_POINTS:
        series.points.append((ep, val))
    else:
        series.points = series.points[::2]          # un point sur deux
        ctx.stride *= 2
        if len(series.points) < IZY_MAX_POINTS:
            series.points.append((ep, val))
    ctx.seen += 1


def hist_close(series, ctx):
    """_histClose() : pointTotal = points reçus, dernier point réel conservé."""
    series.point_total = ctx.seen
    if ctx.have_last and series.points:
        if series.points[-1][0] != ctx.last_ep:
            if len(series.points) < IZY_MAX_POINTS:
                series.points.append((ctx.last_ep, ctx.last_val))
            else:
                series.points[-1] = (ctx.last_ep, ctx.last_val)
    ctx.seen, ctx.stride, ctx.have_last, ctx.have_pending = 0, 1, False, False


def value_at(series, t):
    """valueAt() : interpolation linéaire, (valeur, couvert)."""
    pts = series.points
    if not pts:
        return 0.0, False
    if len(pts) == 1:
        return pts[0][1], (t == pts[0][0])
    if t <= pts[0][0]:
        return pts[0][1], (t == pts[0][0])
    for i in range(1, len(pts)):
        t1, v1 = pts[i]
        if t == t1:
            return v1, True
        if t < t1:
            t0, v0 = pts[i - 1]
            if t1 <= t0 or t0 == 0:
                return v1, False
            k = (t - t0) / (t1 - t0)
            return v0 + (v1 - v0) * k, True
    return pts[-1][1], False


def decode(records):
    """Rejoue un flux (key, valeur) dans l'ordre du JSON du cloud.

    records : liste de tuples ("time", "2026-11-04 07:00") / ("val", 12.5) /
              ("key", "production") / ("unit", "W") … ; les points précèdent
              la "key" de leur série (c'est la clé qui clôt la précédente).
    """
    # Comme dans le header : tableau FIXE de séries, les points sont accumulés
    # dans le slot courant AVANT l'arrivée de sa "key" (qui clôt la série).
    series = [Series() for _ in range(IZY_MAX_SERIES)]
    ctx = HistCtx()
    for k, v in records:
        if k == "key":
            if ctx.slot >= IZY_MAX_SERIES:
                continue
            hist_close(series[ctx.slot], ctx)
            series[ctx.slot].key = v
            ctx.slot += 1
        elif k == "time":
            ctx.pending_ep = epoch_from_datetime(v)
            ctx.have_pending = True
        elif k in ("val", "value"):
            if ctx.slot >= IZY_MAX_SERIES or not ctx.have_pending:
                continue
            hist_append(series[ctx.slot], ctx, ctx.pending_ep, float(v))
            ctx.have_pending = False
        elif ctx.slot > 0:
            s = series[ctx.slot - 1]
            if k == "unit":
                s.unit = v
            elif k == "name":
                s.name = v
            elif k == "lineType":
                s.line_type = v
    # _seriesCount = min(ctx.slot, IZY_MAX_SERIES) : seules les séries dont la
    # clé est arrivée sont publiées.
    return series[:min(ctx.slot, IZY_MAX_SERIES)]


# -------------------------------------------------------------------- tests
checks = 0
fails = []


def ok(cond, label):
    global checks
    checks += 1
    if not cond:
        fails.append(label)


def eq(a, b, label):
    ok(a == b, "%s (obtenu %r, attendu %r)" % (label, a, b))


def main():
    # ---- 1. epoch : granularités complètes et partielles -----------------
    eq(epoch_from_datetime("1970-01-01"), 0, "epoch 1970-01-01 = 0")
    eq(epoch_from_datetime("2026-11-04"), 1793750400, "epoch jour")
    eq(epoch_from_datetime("2026-11-04 14:30"), 1793750400 + 52200, "epoch jour+heure")
    eq(epoch_from_datetime("2026-11-04T14:30:15"), 1793750400 + 52215, "epoch ISO T")
    eq(epoch_from_datetime("2026-11"), epoch_from_datetime("2026-11-01"), "epoch mois partiel")
    eq(epoch_from_datetime("2026"), epoch_from_datetime("2026-01-01"), "epoch année partielle")
    eq(epoch_from_datetime(""), 0, "epoch vide = 0")
    eq(epoch_from_datetime("bidon"), 0, "epoch illisible = 0")
    eq(epoch_from_datetime("1900-01-01"), 0, "epoch hors bornes = 0")

    # ---- 2. normalisation du searchTime par granularité ------------------
    eq(normalize_date(RANGE_DAY, "2026-11-04"), "2026-11-04", "searchTime day")
    eq(normalize_date(RANGE_MONTH, "2026-11-04"), "2026-11", "searchTime month tronqué")
    eq(normalize_date(RANGE_YEAR, "2026-11-04"), "2026", "searchTime year tronqué")
    eq(normalize_date(RANGE_ALL, "2026-11-04"), "2026-11-04", "searchTime all")
    eq(range_name(RANGE_MONTH), "month", "timeType month")
    eq(range_name(99), "day", "timeType inconnu → day")

    # ---- 3. décodage d'une réponse « station » (points AVANT la key) -----
    rec = []
    for h in range(0, 24):
        rec.append(("time", "2026-11-04 %02d:00:00" % h))
        rec.append(("val", 0.0 if h < 7 else (h - 6) * 100.0))
    rec += [("key", "production"), ("name", "Production"), ("unit", "W"), ("lineType", "line")]
    series = decode(rec)
    eq(len(series), 1, "1 série décodée")
    eq(series[0].key, "production", "clé de série")
    eq(series[0].unit, "W", "unité de série")
    eq(len(series[0].points), 24, "24 points conservés")
    eq(series[0].point_total, 24, "pointTotal = 24")
    eq(series[0].points[0][0], epoch_from_datetime("2026-11-04 00:00"), "1er point horodaté")

    # ---- 4. valeur interpolée -------------------------------------------
    v, c = value_at(series[0], epoch_from_datetime("2026-11-04 12:00"))
    eq(v, 600.0, "interpolation sur un point exact")
    ok(c, "point exact couvert")
    v, c = value_at(series[0], epoch_from_datetime("2026-11-04 12:30"))
    eq(v, 650.0, "interpolation au milieu d'un segment")
    ok(c, "milieu couvert")
    v, c = value_at(series[0], epoch_from_datetime("2026-11-04 12:06"))
    eq(round(v, 3), 610.0, "interpolation 1/10 du segment")
    v, c = value_at(series[0], epoch_from_datetime("2026-11-03 23:00"))
    eq(v, 0.0, "avant la plage → 1er point")
    ok(not c, "avant la plage non couvert")
    v, c = value_at(series[0], epoch_from_datetime("2026-11-05 01:00"))
    eq(v, 1700.0, "après la plage → dernier point")
    ok(not c, "après la plage non couvert")

    # cas dégénérés
    empty = Series()
    eq(value_at(empty, 1000), (0.0, False), "série vide")
    single = Series()
    single.points = [(1000, 5.0)]
    eq(value_at(single, 1000), (5.0, True), "point unique (exact)")
    eq(value_at(single, 1001), (5.0, False), "point unique (hors)")
    zero_ts = Series()
    zero_ts.points = [(0, 1.0), (100, 2.0)]
    eq(value_at(zero_ts, 50), (2.0, False), "horodatage nul → pas d'interpolation")
    neg = Series()
    neg.points = [(100, -50.0), (200, 50.0)]
    v, c = value_at(neg, 150)
    eq(v, 0.0, "passage par zéro")

    # ---- 5. décimation adaptative ---------------------------------------
    big = []
    for i in range(1000):
        big.append(("time", "2026-11-04 %02d:%02d:00" % (i // 60, i % 60)))
        big.append(("val", float(i)))
    big.append(("key", "consumption"))
    s = decode(big)
    eq(len(s), 1, "série unique sur gros flux")
    ok(len(s[0].points) <= IZY_MAX_POINTS, "décimation ≤ IZY_MAX_POINTS")
    ok(len(s[0].points) >= IZY_MAX_POINTS // 2, "décimation garde une résolution utile")
    eq(s[0].point_total, 1000, "pointTotal = 1000 reçus")
    ok(s[0].point_total > len(s[0].points), "pointTotal > points conservés (décimé)")
    eq(s[0].points[-1][1], 999.0, "dernier point réel conservé")

    # ---- 6. limite du nombre de séries ----------------------------------
    many = []
    for k in range(IZY_MAX_SERIES + 4):
        many += [("time", "2026-11-04 12:00:00"), ("val", 1.0), ("key", "s%d" % k)]
    s = decode(many)
    eq(len(s), IZY_MAX_SERIES, "IZY_MAX_SERIES respecté")

    # ---- 7. forme « appareil » : data[] à plat, 2 séries -----------------
    rec = [("time", "2026-11-04 12:00:00"), ("val", 230.0), ("key", "v_ac"),
           ("time", "2026-11-04 12:00:00"), ("val", 1.5), ("key", "a_ac")]
    s = decode(rec)
    eq([x.key for x in s], ["v_ac", "a_ac"], "2 séries appareil")
    eq([x.points[0][1] for x in s], [230.0, 1.5], "valeurs par série")

    # ---------------------------------------------------------------- bilan
    print("%d vérifications" % checks)
    if fails:
        print("ÉCHECS :")
        for f in fails:
            print("  - " + f)
        return 1
    print("TOUS LES TESTS PASSENT")
    return 0


if __name__ == "__main__":
    sys.exit(main())
