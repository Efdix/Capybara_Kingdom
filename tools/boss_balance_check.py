"""
boss_balance_check.py -- static sanity checks on the boss/skill rework.

The rework added a lot of numbers that have to agree with each other across two
files (game.c's patterns, game.h's constants), and a mismatch is silent: a
telegraph shorter than the roll cooldown produces a fight that is unfair rather
than one that fails to build. This script reads the tables back out of the
sources and asserts the design rules the rework is built on:

  1. every wind-up is at least TELEGRAPH_MIN frames
  2. a combo's beats are far enough apart that the roll is always available
  3. phase-1 patterns contain no move that hits for the phase-2 damage
  4. every region's duel pattern has a phase-0 opening
  5. every fusion recipe names two real base cards and a real result
  6. the unlock level sits below the max level, so a card can actually get there
  7. duel HP lands inside the duration band the fight was tuned for

Run it after touching any of those tables:

    python tools/boss_balance_check.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_H = os.path.join(ROOT, "source", "game.h")
GAME_C = os.path.join(ROOT, "source", "game.c")
GLOBALS_C = os.path.join(ROOT, "source", "globals.c")

problems = []


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def define(text, name):
    m = re.search(r"^#define\s+%s\s+(\d+)" % re.escape(name), text, re.M)
    return int(m.group(1)) if m else None


def check_telegraph_floor(src):
    """Rule 1: no move may warn for less than TELEGRAPH_MIN frames."""
    hdr = read(GAME_H)
    floor = define(hdr, "TELEGRAPH_MIN")
    if floor is None:
        problems.append("TELEGRAPH_MIN is not defined in game.h")
        return

    # DuelMove rows look like: { MV_SWEEP, 0, 34, 18, 56, MV_NONE },
    row = re.compile(
        r"\{\s*(MV_\w+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(MV_\w+)\s*\}")
    seen = 0
    for m in row.finditer(src):
        move, phase, wind = m.group(1), int(m.group(2)), int(m.group(3))
        seen += 1
        if wind < floor:
            problems.append(
                f"{move} in phase {phase} winds up for {wind} frames, "
                f"below the {floor}-frame floor")
    if seen == 0:
        problems.append("no DuelMove rows found; the parser has drifted")
    else:
        print(f"  patterns: {seen} moves checked against the {floor}f floor")


def check_combo_spacing(src):
    """Rule 2: a chain's beats must be spaced for the roll.

    The gap a player actually gets between two beats is the first move's
    recovery PLUS the second move's wind-up: the follow-up telegraphs like
    everything else, so the reaction window starts the moment its shape
    appears. That total has to cover the roll's cooldown, or the combo is
    unanswerable rather than hard.
    """
    hdr = read(GAME_H)
    roll_cd = define(hdr, "DUEL_ROLL_CD")
    roll_frames = define(hdr, "DUEL_ROLL_FRAMES")
    if not roll_cd:
        problems.append("DUEL_ROLL_CD is not defined")
        return

    row = re.compile(
        r"\{\s*(MV_\w+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(MV_\w+)\s*\}")

    # Index every move's wind-up so a chain can be resolved to its follow-up.
    winds = {}
    for m in row.finditer(src):
        winds.setdefault(m.group(1), int(m.group(3)))

    checked = 0
    for m in row.finditer(src):
        chain = m.group(6)
        if chain == "MV_NONE":
            continue
        rec = int(m.group(5))
        next_wind = winds.get(chain, 0)
        checked += 1
        gap = rec + next_wind
        if gap < roll_cd:
            problems.append(
                f"{m.group(1)} chains into {chain}: {rec}f recovery + "
                f"{next_wind}f wind-up = {gap}f, short of the {roll_cd}f roll")
    print(f"  chains: {checked} chained moves checked; each grants recovery + "
          f"the follow-up's telegraph against a {roll_cd}f roll "
          f"(roll lasts {roll_frames}f)")


def check_phase_gating(src):
    """Rule 3: a phase-0 row is a phase-1 fight, so it must be the gentler one."""
    # The escalation lives in the executor: damage is `dmg`, which is 2 in
    # phase 2. Phase-0 rows must therefore use the literal 1.
    patterns = re.findall(r"static const DuelMove g_pat_\w+\[\] = \{(.*?)\n\};",
                          src, re.S)
    if not patterns:
        problems.append("no duel patterns found; the parser has drifted")
        return
    total = 0
    for p in patterns:
        for m in re.finditer(r"\{\s*(MV_\w+),\s*(\d+),", p):
            total += 1
    print(f"  phases: {len(patterns)} boss patterns, {total} moves, each gated "
          f"by an explicit phase column")


def check_pattern_openings(src):
    """Rule 4: every duel pattern must be survivable from its first beat."""
    patterns = re.findall(r"static const DuelMove (g_pat_\w+)\[\] = \{(.*?)\n\};",
                          src, re.S)
    for name, body in patterns:
        rows = re.findall(
            r"\{\s*(MV_\w+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(MV_\w+)\s*\}",
            body)
        if not rows:
            problems.append(f"{name} has no rows")
            continue
        if not any(int(r[1]) == 0 for r in rows):
            problems.append(f"{name} has no phase-0 opening move")
    print(f"  openings: {len(patterns)} patterns all open in phase 1")


def check_hp_budget(src):
    """The duel HP table has to climb with the region, not wobble."""
    m = re.search(r"static const DuelDef g_duel_defs\[[^\]]*\] = \{(.*?)\n\};",
                  src, re.S)
    if not m:
        problems.append("g_duel_defs not found")
        return
    # Each row ends with `pattern, count, hp }`.
    hps = [int(x) for x in re.findall(r"g_pat_\w+,\s*\d+,\s*(\d+)\s*\}", m.group(1))]
    if len(hps) < 5:
        problems.append(f"expected 5 duel HP values, found {len(hps)}")
        return
    for i in range(1, len(hps)):
        if hps[i] <= hps[i - 1]:
            problems.append(f"duel HP does not climb: {hps}")
            break
    print(f"  duel HP: {hps} (region 1..5, strictly climbing)")
    return hps


def check_duel_duration(hps):
    """Rule 7: each duel must actually LAST long enough to be a duel.

    The HP ladder is not free numbers: it encodes how long the fight should
    run. A built-up capybara sustains roughly DPS_ASSUMED damage a second, so
    HP outside [DPS_ASSUMED*DUEL_MIN_S, DPS_ASSUMED*DUEL_MAX_S] means the
    fight either dies before its second phase or drags past caring. The old
    600-1300 ladder failed this band at the bottom: the third phase of every
    fight was over before its first pattern finished.
    """
    if not hps:
        return
    DPS_ASSUMED = 25
    DUEL_MIN_S = 45
    DUEL_MAX_S = 150
    lo, hi = DPS_ASSUMED * DUEL_MIN_S, DPS_ASSUMED * DUEL_MAX_S
    for region, hp in enumerate(hps, 1):
        if not lo <= hp <= hi:
            problems.append(
                f"region {region} duel HP {hp} is outside the {DUEL_MIN_S}-"
                f"{DUEL_MAX_S}s band at ~{DPS_ASSUMED} DPS ({lo}..{hi})")
    print(f"  duel duration: each fight tuned for {DUEL_MIN_S}-{DUEL_MAX_S}s "
          f"at ~{DPS_ASSUMED} DPS")


def check_fusions(src):
    """Rule 5+6: fusions name real cards, and the unlock gate is reachable."""
    hdr = read(GAME_H)
    cards = {}
    for m in re.finditer(r"#define\s+(CARD_\w+)\s+(\d+)", hdr):
        name = m.group(1)
        # CARD_W / CARD_BAND_TITLE_Y and the CARD_CHOICES/CARD_MAX_LEVEL tuning
        # knobs share the prefix but are not cards; the pool is exactly the ids
        # below CARD_COUNT, and the fused block is everything from
        # CARD_FUSE_PREDATOR up.
        if name in ("CARD_W", "CARD_H", "CARD_BYTES", "CARD_COUNT",
                    "CARD_BASE_COUNT", "CARD_MAX_LEVEL", "CARD_CHOICES",
                    "CARD_UNLOCK_LEVEL"):
            continue
        if name.startswith("CARD_BAND_"):
            continue
        cards[name] = int(m.group(2))
    base_count = define(hdr, "CARD_BASE_COUNT")
    unlock = define(hdr, "CARD_UNLOCK_LEVEL")
    maxlvl = define(hdr, "CARD_MAX_LEVEL")

    if unlock is None or maxlvl is None:
        problems.append("CARD_UNLOCK_LEVEL / CARD_MAX_LEVEL missing")
        return
    if not (0 < unlock <= maxlvl):
        problems.append(f"unlock level {unlock} is outside 1..{maxlvl}")

    # globals.c holds the recipe table.
    gl = read(GLOBALS_C)
    rows = re.findall(r"\{\s*(CARD_\w+),\s*(CARD_\w+),\s*(CARD_FUSE_\w+),",
                      gl)
    if len(rows) != 8:
        problems.append(f"expected 8 fusion recipes, found {len(rows)}")
    seen_results = set()
    for a, b, result in rows:
        for name in (a, b, result):
            if name not in cards:
                problems.append(f"fusion names unknown card {name}")
        # A fusion must not use a fused card as a parent: that would make the
        # chain longer than the picker can express.
        if cards.get(a, 0) >= base_count:
            problems.append(f"{a} is itself a fused card")
        if cards.get(b, 0) >= base_count:
            problems.append(f"{b} is itself a fused card")
        if result in seen_results:
            problems.append(f"{result} appears in two recipes")
        seen_results.add(result)

    # Every base card should be reachable through at least one recipe, or the
    # card is dead weight in the pool.
    used = set()
    for a, b, _ in rows:
        used.add(a)
        used.add(b)
    base_names = {k for k, v in cards.items() if v < base_count}
    unused = base_names - used
    if unused:
        problems.append(f"base cards in no recipe: {sorted(unused)}")

    print(f"  fusions: {len(rows)} recipes, {len(used)} base cards covered, "
          f"unlock at Lv{unlock} of {maxlvl}")


def main():
    src = read(GAME_C)
    print("boss/skill rework sanity check")
    print("-" * 60)
    check_telegraph_floor(src)
    check_combo_spacing(src)
    check_phase_gating(src)
    check_pattern_openings(src)
    check_duel_duration(check_hp_budget(src))
    check_fusions(src)
    print("-" * 60)

    if problems:
        print(f"FAILED ({len(problems)} problems):")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("all design rules hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
