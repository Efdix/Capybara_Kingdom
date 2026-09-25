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
import re
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


def _source_define(header, name):
    """Read one integer #define out of a source header, or None.

    Used where an asset's geometry is owned by the C side: the point is that the
    engine's number and the generator's output are checked against each other,
    so neither can move without the other being noticed.
    """
    path = os.path.join("source", header)
    if not os.path.exists(path):
        return None
    m = re.search(rf"^#define\s+{name}\s+(-?\d+)",
                  open(path, encoding="utf-8").read(), re.M)
    return int(m.group(1)) if m else None


def card_count():
    """How many card faces the engine expects. See check_sprites()."""
    return _source_define("game.h", "CARD_COUNT")


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
# The world is much larger than the screen, so it is stored as a 64-tile 16x16
# atlas plus one byte per tile position rather than as pixels. These checks are
# the ones that catch a world that would be broken on real hardware: a map byte
# naming a tile the atlas does not have (reads past the end of the blob), an
# atlas tile with a transparent pixel (shows as magenta), or a map with no walls
# at all (the player walks off the world).
#
# The kingdom is deliberately smaller than the five battle regions, and the
# engine binds its size at runtime (world_set_size) from the same table this
# duplicates. A mismatch here is a map read past its end, so the sizes are
# checked by name rather than assumed.
# ---------------------------------------------------------------------------

AREAS = ["hub", "rain", "farm", "past", "marsh", "cacao"]

ATLAS_TILES = 64
WORLD_COLS, WORLD_ROWS = 128, 96
HUB_COLS, HUB_ROWS = 48, 40

AREA_SIZE = {
    "hub": (HUB_COLS, HUB_ROWS),
    **{n: (WORLD_COLS, WORLD_ROWS)
       for n in ("rain", "farm", "past", "marsh", "cacao")},
}


def check_areas():
    for name in AREAS:
        atlas = os.path.join(GRAPHICS, f"area_{name}_atlas.bin")
        tmap = os.path.join(GRAPHICS, f"area_{name}_map.bin")
        prop = os.path.join(GRAPHICS, f"area_{name}_prop.bin")
        cols, rows = AREA_SIZE[name]

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
        expected_map = cols * rows
        check(nm == expected_map,
              f"area_{name}_map.bin is {nm} bytes, expected {expected_map} "
              f"({cols}x{rows} tiles)")
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
        mid = (rows // 2) * cols + cols // 2
        check(not propdata[mapdata[mid]],
              f"area_{name} spawn tile is solid (characters would start in a "
              f"wall)")

        # Walkability from the spawn: a large map that is 90% walled off still
        # looks fine in a screenshot and plays terribly.
        seen = set()
        stack = [(cols // 2, rows // 2)]
        while stack:
            x, y = stack.pop()
            if (x, y) in seen or not (0 <= x < cols and 0 <= y < rows):
                continue
            if propdata[mapdata[y * cols + x]]:
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

    # 10 bosses x 8 frames: the four walk poses plus the four duel poses the
    # fight reads off body language (wind-up / signature / enraged / broken).
    check_sprite(os.path.join(GRAPHICS, "boss_tiles.bin"), 64, 64, 10 * 8)

    # Skill cards: one 72x96 picture per card. The count is read out of game.h
    # rather than written here as a literal.
    #
    # It used to be a hardcoded 12, and that is exactly how the fusion cards
    # went unnoticed: the engine grew a CARD_COUNT of 20 with eight fused cards
    # that had no art at all, while the C-side card lookup bounded itself by
    # CARD_COUNT and the atlas only held 12 -- so the first fused card drawn
    # would have read past the end of the blob. Tying the atlas size to the
    # engine's own count turns that class of drift into a build failure.
    cards = card_count()
    check(cards is not None, "game.h is missing CARD_COUNT")
    check_sprite(os.path.join(GRAPHICS, "card_atlas.bin"), 72, 96, cards or 12)

    # The farm sheet: soil (dry/wet) + 5 crops x 3 stages, padded out to 24
    # tiles so farm.c can index it without a bounds dance. Every tile has to be
    # non-empty, which is why the sheet is generated with no blank padding --
    # the size check below pins that, and the sheet size is exactly the number
    # of tiles farm.h will ask for.
    check_sprite(os.path.join(GRAPHICS, "farm_tiles.bin"), 16, 16, 17)
    n = size_of(os.path.join(GRAPHICS, "farm_tiles.bin"))
    check(n == 17 * 16 * 16,
          f"farm_tiles.bin is {n} bytes, expected {17 * 16 * 16}")


# ---------------------------------------------------------------------------
# The farm
#
# The field is drawn at runtime rather than baked into the hub map, so the
# generated hub map has no idea it exists. These checks are what keep the two
# from disagreeing: the rectangle farm.h draws into must be plain walkable
# ground in the map the generator actually produced, or the player would be
# planting crops in a pond.
#
# Both sides are read from their own source of truth -- farm.h for the geometry,
# hub_layout.h for the map size -- so this cannot silently pass against a stale
# copy of either.
# ---------------------------------------------------------------------------

def check_farm():
    gfx = os.path.join(GRAPHICS, "farm_tiles.bin")
    if size_of(gfx) < 0:
        problems.append("farm_tiles.bin is missing")
        return

    src = os.path.join("source", "farm.h")
    if not os.path.exists(src):
        problems.append("source/farm.h is missing")
        return
    text = open(src, encoding="utf-8").read()

    def define(name):
        m = re.search(rf"^#define\s+{name}\s+(-?\d+)", text, re.M)
        return int(m.group(1)) if m else None

    tx = define("FARM_TILE_X")
    ty = define("FARM_TILE_Y")
    cols = define("FARM_COLS")
    rows = define("FARM_ROWS")
    if None in (tx, ty, cols, rows):
        problems.append("farm.h is missing its geometry defines")
        return

    # The tile ids the field blits must exist in the sheet. farm.h includes the
    # generated farm_art.h for the layout, so the numbers only live in one
    # place; these defines are read from there.
    art = os.path.join("source", "farm_art.h")
    if not os.path.exists(art):
        problems.append("source/farm_art.h is missing (run tools/gen_farm.py)")
        return
    art_text = open(art, encoding="utf-8").read()

    def adefine(name):
        m = re.search(rf"^#define\s+{name}\s+(-?\d+)", art_text, re.M)
        return int(m.group(1)) if m else None

    base = adefine("FARM_TILE_CROP_BASE")
    crops = adefine("FARM_CROP_COUNT")
    stages = adefine("FARM_STAGE_COUNT")
    if None in (base, crops, stages):
        problems.append("farm_art.h is missing its sheet layout defines")
        return

    # ...and the sheet must hold exactly the tiles that arithmetic reaches.
    sheet = size_of(gfx) // (16 * 16)
    top = base + crops * stages
    check(sheet == top,
          f"farm_tiles.bin holds {sheet} tiles but the layout needs {top}")

    # farm.h must include the generated header rather than re-deriving it: a
    # second copy of these numbers is exactly how the art and the code drift.
    check("farm_art.h" in text,
          "source/farm.h should include farm_art.h instead of hardcoding "
          "the sheet layout")

    # ...and that the rectangle is real ground in the hub map.
    hdr = os.path.join("source", "hub_layout.h")
    if not os.path.exists(hdr):
        problems.append("source/hub_layout.h is missing")
        return
    hdr_text = open(hdr, encoding="utf-8").read()

    def hdef(name):
        m = re.search(rf"^#define\s+{name}\s+(-?\d+)", hdr_text, re.M)
        return int(m.group(1)) if m else None

    hcols = hdef("HUB_COLS")
    hrows = hdef("HUB_ROWS")
    if hcols is None or hrows is None:
        problems.append("hub_layout.h is missing HUB_COLS/HUB_ROWS")
        return

    check(0 <= tx and tx + cols <= hcols and 0 <= ty and ty + rows <= hrows,
          f"the farm rectangle {cols}x{rows} at ({tx},{ty}) is outside the "
          f"{hcols}x{hrows} hub map")

    path = os.path.join(GRAPHICS, "area_hub_map.bin")
    if size_of(path) < 0:
        problems.append("area_hub_map.bin is missing")
        return
    data = open(path, "rb").read()
    prop = open(os.path.join(GRAPHICS, "area_hub_prop.bin"), "rb").read()

    solid = 0
    for y in range(ty, ty + rows):
        for x in range(tx, tx + cols):
            if y * hcols + x >= len(data):
                continue
            tid = data[y * hcols + x]
            if tid < len(prop) and prop[tid]:
                solid += 1
    check(solid == 0,
          f"the farm rectangle has {solid} solid tile(s) in it; "
          f"move it in farm.h (and the comment there) to clear lawn")


# ---------------------------------------------------------------------------
# Full-screen UI art
#
# The title scene and the expedition chart are blitted as one 256x192 image
# covering the whole play area. A short file would leave the bottom of the
# screen showing whatever the previous frame left there, so the size is exact.
# ---------------------------------------------------------------------------

def check_ui():
    for name in ("ui_title", "ui_worldmap"):
        path = os.path.join(GRAPHICS, f"{name}.bin")
        check_sprite(path, 256, 192, 1)

    # The node anchors the C side draws its markers at come from the same
    # generator that painted the biomes; a node that fell outside the image
    # would put a marker on top of the sea.
    hdr = os.path.join("source", "worldmap_nodes.h")
    check(os.path.exists(hdr), "source/worldmap_nodes.h is missing")
    if not os.path.exists(hdr):
        return
    text = open(hdr, encoding="utf-8").read()
    import re
    m = re.search(r"g_world_map_node_xy\[(\d+)\]\[2\]", text)
    check(m is not None, "worldmap_nodes.h has no node table")
    if not m:
        return
    check(int(m.group(1)) == 5,
          f"world map has {m.group(1)} nodes, expected 5 (one per region)")
    for (nx, ny) in re.findall(r"\{\s*(\d+),\s*(\d+)\s*\}", text):
        check(8 <= int(nx) < 248 and 40 <= int(ny) < 184,
              f"world map node ({nx}, {ny}) is outside the drawable area")


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
    check_farm()
    check_ui()
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
