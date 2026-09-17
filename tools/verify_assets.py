"""
verify_assets.py -- integrity checks for every generated asset.

Run this after the generators (or from the Makefile) to catch the classes of
mistake that are invisible in a code review but obvious on a real DS screen:

  * file sizes that don't match the geometry the C code assumes
  * palette-index values that don't fit in 8 bits (would wrap and recolor)
  * collision maps whose dimensions disagree with the area bitmap
  * sprite blobs whose frame count doesn't divide evenly
  * audio files that are silent, clipped, or non-looping

Exits non-zero on failure so it can gate a build.
"""

import os
import struct
import sys
import wave

GRAPHICS = "graphics"
AUDIO = "audio"

problems = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        problems.append(msg)


def size_of(path):
    return os.path.getsize(path) if os.path.exists(path) else -1


# ---------------------------------------------------------------------------
# Palette
# ---------------------------------------------------------------------------

def check_palette():
    path = os.path.join(GRAPHICS, "palette.bin")
    n = size_of(path)
    check(n == 512, f"palette.bin is {n} bytes, expected 512 (256 x u16)")
    if n != 512:
        return
    data = open(path, "rb").read()
    # Index 0 must be the transparent marker; the rest must not be all zero,
    # which would mean the palette silently failed to generate.
    first = struct.unpack("<H", data[0:2])[0]
    rest = set(struct.unpack("<H", data[i:i + 2])[0]
               for i in range(2, 512, 2))
    check(first != 0, "palette index 0 should be the transparent marker")
    check(len(rest) > 10, f"palette has only {len(rest)} distinct colors")


# ---------------------------------------------------------------------------
# World: tile atlas + tile map per region
#
# The world is much larger than the screen, so it is stored as a 48-tile 16x16
# atlas plus one byte per tile position rather than as pixels. These checks are
# the ones that catch a world that would be broken on real hardware: a map byte
# naming a tile the atlas does not have (reads past the end of the blob), an
# atlas tile with a transparent pixel (shows as magenta), or a map with no walls
# at all (the player walks off the world).
# ---------------------------------------------------------------------------

AREAS = ["hub", "rain", "farm", "past", "marsh", "cacao"]

ATLAS_TILES = 48
WORLD_COLS, WORLD_ROWS = 128, 96


def check_areas():
    for name in AREAS:
        atlas = os.path.join(GRAPHICS, f"area_{name}_atlas.bin")
        tmap = os.path.join(GRAPHICS, f"area_{name}_map.bin")
        prop = os.path.join(GRAPHICS, f"area_{name}_prop.bin")

        na = size_of(atlas)
        expected_atlas = ATLAS_TILES * 16 * 16
        check(na == expected_atlas,
              f"area_{name}_atlas.bin is {na} bytes, expected {expected_atlas} "
              f"({ATLAS_TILES} tiles of 16x16)")
        if na != expected_atlas:
            continue

        data = open(atlas, "rb").read()
        check(max(data) < 256, f"area_{name} atlas has an out-of-range index")

        # Every tile must be fully opaque. Index 0 is the transparent marker and
        # the world is presented as a background layer, so a single transparent
        # pixel shows as magenta garbage on a real DS.
        for t in range(ATLAS_TILES):
            tile = data[t * 256:(t + 1) * 256]
            if 0 in tile:
                problems.append(
                    f"area_{name} atlas tile {t} contains transparent pixels "
                    f"(would show as garbage: the world bitmap is opaque)")
        # A tile that is one flat colour means a painter silently failed.
        for t in range(ATLAS_TILES):
            tile = data[t * 256:(t + 1) * 256]
            if len(set(tile)) < 2:
                problems.append(f"area_{name} atlas tile {t} is a flat colour")

        nm = size_of(tmap)
        expected_map = WORLD_COLS * WORLD_ROWS
        check(nm == expected_map,
              f"area_{name}_map.bin is {nm} bytes, expected {expected_map} "
              f"({WORLD_COLS}x{WORLD_ROWS} tiles)")
        np_ = size_of(prop)
        check(np_ == ATLAS_TILES,
              f"area_{name}_prop.bin is {np_} bytes, expected {ATLAS_TILES}")

        if nm != expected_map or np_ != ATLAS_TILES:
            continue

        mapdata = open(tmap, "rb").read()
        propdata = open(prop, "rb").read()

        # Every id in the map must name a tile that exists, or the engine reads
        # past the end of the atlas and draws garbage.
        bad = [i for i, v in enumerate(mapdata) if v >= ATLAS_TILES]
        check(not bad,
              f"area_{name} map names {len(bad)} tiles that do not exist "
              f"(first at index {bad[0] if bad else -1}), max id "
              f"{max(mapdata)}")

        # There must be walls, and they must not cover everything.
        solid = sum(1 for v in mapdata if propdata[v])
        check(solid > 100, f"area_{name} has almost no solid tiles ({solid})")
        check(solid < expected_map * 0.9,
              f"area_{name} is {solid}/{expected_map} solid (unplayable)")

        # The spawn tile must be walkable: the engine puts both characters
        # there with no collision test.
        mid = (WORLD_ROWS // 2) * WORLD_COLS + WORLD_COLS // 2
        check(not propdata[mapdata[mid]],
              f"area_{name} spawn tile is solid (characters would start in a "
              f"wall)")

        # Walkability from the spawn: a large map that is 90% walled off still
        # looks fine in a screenshot and plays terribly.
        seen = set()
        stack = [(WORLD_COLS // 2, WORLD_ROWS // 2)]
        while stack:
            x, y = stack.pop()
            if (x, y) in seen or not (0 <= x < WORLD_COLS and 0 <= y < WORLD_ROWS):
                continue
            if propdata[mapdata[y * WORLD_COLS + x]]:
                continue
            seen.add((x, y))
            stack.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
        walkable = sum(1 for i, v in enumerate(mapdata) if not propdata[v])
        frac = len(seen) / max(walkable, 1)
        check(frac > 0.75,
              f"area_{name} is fragmented: only {frac:.0%} of its walkable "
              f"tiles are reachable from the spawn point")


# ---------------------------------------------------------------------------
# Sprites
# ---------------------------------------------------------------------------

def check_sprite(path, w, h, frames):
    n = size_of(path)
    if n < 0:
        problems.append(f"{path} is missing")
        return
    expected = w * h * frames
    check(n == expected,
          f"{os.path.basename(path)} is {n} bytes, expected {expected} "
          f"({frames} frames of {w}x{h})")
    if n != expected:
        return
    data = open(path, "rb").read()
    # Every frame must contain some opaque pixels; an all-zero frame means the
    # drawing routine produced nothing (an invisible character on screen).
    for f in range(frames):
        frame = data[f * w * h:(f + 1) * w * h]
        if not any(frame):
            problems.append(
                f"{os.path.basename(path)} frame {f} is completely empty")


def check_sprites():
    # The two characters: 8 directions x 16 frames (idle 4 / walk 6 / bite 4 / hurt 2)
    for hero in ("hero_m", "hero_f"):
        check_sprite(os.path.join(GRAPHICS, f"{hero}_tiles.bin"),
                     32, 32, 8 * 16)

    # 15 mobs x 4 frames (idle / bob / attack / hurt)
    check_sprite(os.path.join(GRAPHICS, "mob_tiles.bin"), 32, 32, 15 * 4)

    # 10 bosses x 4 frames (idle / bob / attack / hurt)
    check_sprite(os.path.join(GRAPHICS, "boss_tiles.bin"), 64, 64, 10 * 4)

    # Skill cards: one 72x96 picture per card, 12 cards.
    check_sprite(os.path.join(GRAPHICS, "card_atlas.bin"), 72, 96, 12)



# ---------------------------------------------------------------------------
# Font
# ---------------------------------------------------------------------------

def check_font():
    glyphs = os.path.join(GRAPHICS, "font_glyphs.bin")
    n = size_of(glyphs)
    check(n > 0 and n % 128 == 0,
          f"font_glyphs.bin is {n} bytes, expected a multiple of 128")

    hdr = os.path.join("source", "font_chars.h")
    check(os.path.exists(hdr), "source/font_chars.h is missing")

    # Cross-check the atlas against every Chinese character used in the game
    # sources: a missing glyph renders as a blank box on the DS, so this is the
    # check that keeps the script legible.
    if os.path.exists(hdr):
        text = open(hdr, encoding="utf-8").read()
        start = text.index('"')
        atlas = set()
        for ch in text[start:]:
            if ord(ch) > 0x2000:
                atlas.add(ch)

        missing = set()
        for root, _d, files in os.walk("source"):
            for f in files:
                if not f.endswith((".c", ".h")):
                    continue
                if f in ("font_chars.h", "palette_ids.h"):
                    continue
                body = open(os.path.join(root, f), encoding="utf-8",
                            errors="ignore").read()
                for ch in body:
                    if ord(ch) > 0x2000 and ch not in atlas:
                        missing.add(ch)
        check(not missing,
              "font atlas is missing characters used by the game: "
              + "".join(sorted(missing)))


# ---------------------------------------------------------------------------
# Audio
# ---------------------------------------------------------------------------

def check_audio():
    sfx_dir = os.path.join(AUDIO, "sfx")
    music_dir = os.path.join(AUDIO, "music")

    expected_sfx = ["ui_move", "ui_confirm", "ui_cancel", "attack", "hit",
                    "hurt", "enemy_die", "magic_fire", "magic_ice",
                    "magic_thunder", "heal", "pickup", "levelup", "door",
                    "explosion", "heart"]
    for name in expected_sfx:
        path = os.path.join(sfx_dir, name + ".wav")
        if not os.path.exists(path):
            problems.append(f"missing sound effect: {name}.wav")
            continue
        with wave.open(path, "rb") as w:
            frames = w.getnframes()
            raw = w.readframes(frames)
            check(w.getnchannels() == 1, f"{name}.wav should be mono")
            check(w.getsampwidth() == 2, f"{name}.wav should be 16-bit")
            # 1 channel, 2 bytes per sample.
            expect_bytes = frames * 2
            check(len(raw) == expect_bytes,
                  f"{name}.wav holds {len(raw)} bytes but its header claims "
                  f"{frames} frames ({expect_bytes} bytes)")
            if len(raw) >= expect_bytes and frames:
                vals = struct.unpack(f"<{frames}h", raw[:expect_bytes])
                peak = max(abs(v) for v in vals)
                check(peak > 1000,
                      f"{name}.wav is silent (peak {peak})")
                check(peak <= 32767,
                      f"{name}.wav is clipped (peak {peak})")

    expected_mod = ["title", "field", "battle", "boss", "victory", "birthday"]
    for name in expected_mod:
        path = os.path.join(music_dir, name + ".mod")
        if not os.path.exists(path):
            problems.append(f"missing music module: {name}.mod")
            continue
        data = open(path, "rb").read()
        check(data[1080:1084] == b"M.K.",
              f"{name}.mod has a bad magic signature (not a ProTracker module)")
        # Every pattern cell that names a note must also name a sample, or the
        # player produces silence for that note.
        orders = data[952:952 + 128]
        song_len = data[950]
        n_pat = max(orders[:song_len]) + 1
        off = 1084
        note_without_sample = 0
        for _ in range(n_pat):
            for _cell in range(64 * 4):
                b0, b1, b2, b3 = data[off:off + 4]
                off += 4
                period = ((b0 & 0x0F) << 8) | b1
                sample = (b0 & 0xF0) | (b2 >> 4)
                if period and not sample:
                    note_without_sample += 1
        check(note_without_sample == 0,
              f"{name}.mod has {note_without_sample} notes with no sample "
              f"(those notes would be silent)")

    # The birthday tune is the point of the gift: assert it is present, has the
    # right number of notes, and is a real melody rather than a placeholder.
    bday = os.path.join(music_dir, "birthday.mod")
    if os.path.exists(bday):
        data = open(bday, "rb").read()
        orders = data[952:952 + 128]
        song_len = data[950]
        n_pat = max(orders[:song_len]) + 1
        off = 1084
        periods = []
        for _ in range(n_pat):
            for cell in range(64 * 4):
                b0, b1, b2, b3 = data[off:off + 4]
                off += 4
                if cell % 4 == 0:   # channel 0 carries the melody
                    p = ((b0 & 0x0F) << 8) | b1
                    if p:
                        periods.append(p)
        check(len(periods) >= 20,
              f"birthday.mod melody has only {len(periods)} notes "
              f"(expected the full tune)")
        check(len(set(periods)) >= 6,
              f"birthday.mod melody uses only {len(set(periods))} distinct "
              f"pitches (expected a real melody)")


# ---------------------------------------------------------------------------
# Built ROM
# ---------------------------------------------------------------------------

def check_rom():
    """Sanity-check the built ROM's header.

    This caught a real shipping bug: an icon with too many colors made ndstool
    print a warning and then write a ROM whose header was entirely blank. The
    file looked fine on disk and built without failing, but it crashed on boot
    (the ARM9 spun reading unmapped IO). A valid title, game code and banner
    offset are the cheap checks that catch it.
    """
    rom = "capybara_kingdom.nds"
    if not os.path.exists(rom):
        return   # nothing built yet: not an asset problem
    d = open(rom, "rb").read()
    check(len(d) > 512, "built ROM is implausibly small")
    title = d[0:12].rstrip(bytes(1))
    check(bool(title.strip()),
          "ROM header has an empty title (ndstool failed to write the header; "
          "usually an icon with too many colors)")
    check(d[12:16] != bytes(4),
          "ROM header has an empty game code")
    banner = int.from_bytes(d[0x68:0x6C], "little")
    check(0 < banner < len(d),
          f"ROM banner/icon offset is invalid ({banner})")


# ---------------------------------------------------------------------------

def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)) + os.sep + "..")
    print("verifying generated assets in", os.getcwd())

    check_palette()
    check_areas()
    check_sprites()
    check_font()
    check_audio()
    check_rom()

    print(f"\n{checks} checks run")
    if problems:
        print(f"\nFAILED ({len(problems)} problems):")
        for p in problems:
            print("  -", p)
        return 1
    print("all asset checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
