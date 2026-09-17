"""
art_review.py -- objective visual review toolkit for Capybara Kingdom art.

The point of this file: judging pixel art by eye alone is unreliable, and
"looks fine to me" is not a review. Each check below is a criterion that can
fail visibly, so art gets fixed instead of shipping.

Checks implemented:

  1. silhouette()   Fill every opaque pixel with one color. If the creature is
                    not identifiable from the black shape alone, the pose is
                    mush and must be redesigned. This is the single most useful
                    pixel-art test -- detail cannot save a bad silhouette.

  2. at_scale()     Render at 1x, 2x, 4x. The DS displays these sprites at 1x on
                    a 256x192 screen, so anything that does not read at 1x is
                    effectively invisible detail. Reviewing only at 4x-8x
                    (which is what the earlier previews did) hides this.

  3. in_context()   Composite the sprite onto the real area background at the
                    real palette, at 1x and 3x. A sprite that reads on a
                    checkerboard can still vanish against a busy forest.

  4. palette_report() Count distinct colors per sprite, list which global
                    palette entries are actually used, and flag the failures
                    that cause a flat look:
                      - too few shades for the sprite's size (no light/shadow
                        separation)
                      - no value ramp (all colors at similar brightness, so the
                        sprite has no readable form)

Usage:
    python art_review.py                 # review every sprite sheet
    python art_review.py hero_sword      # review one sheet by name
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import art_spec as spec
import gen_common as gc
from gen_common import PAL, PALETTE_NAMES, _RGB


# ---------------------------------------------------------------------------
# Loading sprite sheets the same way the game does
# ---------------------------------------------------------------------------

class Sheet:
    """A flat 8bpp frame blob plus its geometry (mirrors the C Sheet struct)."""

    def __init__(self, name, path, w, h, frames):
        self.name = name
        self.w = w
        self.h = h
        with open(path, "rb") as f:
            self.data = f.read()

        # Derive the real frame count from the file size when it disagrees with
        # the spec. A not-yet-upgraded asset then gets reviewed as it actually
        # is and reported as a mismatch, instead of aborting the whole run and
        # hiding every other problem behind one exception.
        per = w * h
        if len(self.data) % per:
            raise ValueError(
                f"{name}: {len(self.data)} bytes is not a whole number of "
                f"{w}x{h} frames")
        self.actual_frames = len(self.data) // per
        self.frames = frames
        self.matches_spec = (self.actual_frames == frames)

    def frame_pixels(self, index):
        """Row-major palette indices for one frame."""
        base = index * self.w * self.h
        return self.data[base:base + self.w * self.h]

    def canvas(self, index):
        c = gc.Canvas(self.w, self.h)
        px = self.frame_pixels(index)
        c.px = list(px)
        return c


# Sheet registry: keep in sync with tools/gen_heroes.py and gen_monsters.py.
def discover_sheets(graphics="graphics"):
    """All sheets the game loads, with geometry taken from art_spec.

    Geometry is read from art_spec rather than hardcoded here: when this file
    kept its own copy of the dimensions it silently reviewed the old 16x16 art
    while the generator had already moved to 24x24, so the review passed art that
    no longer existed.
    """
    out = []
    for tag in ("hero_sword", "hero_gun"):
        p = os.path.join(graphics, f"{tag}_tiles.bin")
        if os.path.exists(p):
            out.append(Sheet(tag, p, spec.HERO_W, spec.HERO_H,
                             spec.HERO_TOTAL_FRAMES))
    for tag in ("slime", "scorpion", "wasp", "snowball", "emberling",
                "cactus", "lavaball", "dragonfly"):
        p = os.path.join(graphics, f"mob_{tag}.bin")
        if os.path.exists(p):
            out.append(Sheet(f"mob_{tag}", p, spec.MOB_W, spec.MOB_H,
                             spec.MOB_FRAMES))
    for tag in ("treant", "sandworm", "icewolf", "lavagolem", "cakelord"):
        p = os.path.join(graphics, f"boss_{tag}.bin")
        if os.path.exists(p):
            out.append(Sheet(f"boss_{tag}", p, spec.BOSS_W, spec.BOSS_H,
                             spec.BOSS_FRAMES))
    return out


# ---------------------------------------------------------------------------
# Color helpers
# ---------------------------------------------------------------------------

def idx_to_rgb(i):
    if i == 0:
        return (255, 0, 255)
    for name in PALETTE_NAMES:
        if PAL[name] == i:
            return _RGB[name]
    return (0, 0, 0)


def luminance(rgb):
    """Perceived brightness, 0..255 (Rec. 601)."""
    r, g, b = rgb
    return 0.299 * r + 0.587 * g + 0.114 * b


def name_of_index(i):
    if i == 0:
        return "transparent"
    for name in PALETTE_NAMES:
        if PAL[name] == i:
            return name
    return f"?{i}"


# ---------------------------------------------------------------------------
# Check 1: silhouette
# ---------------------------------------------------------------------------

def silhouette_canvas(canvas, color=(20, 20, 24)):
    """Return a PIL image where every opaque pixel is one flat color."""
    from PIL import Image
    img = Image.new("RGBA", (canvas.w, canvas.h), (0, 0, 0, 0))
    px = img.load()
    for y in range(canvas.h):
        for x in range(canvas.w):
            if canvas.get(x, y) != 0:
                px[x, y] = (color[0], color[1], color[2], 255)
    return img


def silhouette_report(canvas):
    """Metrics that predict whether a silhouette will read.

    Returns a dict with:
      fill_ratio   opaque / total pixels. Very low = thin and lost; very high
                   (near 1.0) = a solid blob with no internal shape.
      touches_edge true if the art is clipped by the frame, which on the DS
                   means visible cropping against neighbors.
      bbox         bounding box of opaque pixels.
    """
    xs, ys = [], []
    n_opaque = 0
    for y in range(canvas.h):
        for x in range(canvas.w):
            if canvas.get(x, y) != 0:
                n_opaque += 1
                xs.append(x)
                ys.append(y)
    if not xs:
        return dict(empty=True, fill_ratio=0.0, touches_edge=True,
                    bbox=(0, 0, 0, 0))
    bbox = (min(xs), min(ys), max(xs), max(ys))
    touches = (bbox[0] == 0 or bbox[1] == 0 or
               bbox[2] == canvas.w - 1 or bbox[3] == canvas.h - 1)
    return dict(empty=False,
                fill_ratio=n_opaque / float(canvas.w * canvas.h),
                touches_edge=touches,
                bbox=bbox,
                n_opaque=n_opaque)


# ---------------------------------------------------------------------------
# Check 4: palette / value ramp
# ---------------------------------------------------------------------------

def palette_report(canvas):
    """Distinct colors and whether they form a usable value ramp.

    A sprite whose colors are all at similar brightness reads as a flat
    sticker: there is no light/shadow separation, so the eye cannot parse
    volume. We measure the spread of luminance across the sprite's colors.
    """
    used = {}
    for v in canvas.px:
        if v == 0:
            continue
        used[v] = used.get(v, 0) + 1

    lums = sorted(luminance(idx_to_rgb(i)) for i in used)
    if len(lums) < 2:
        spread = 0.0
    else:
        spread = lums[-1] - lums[0]
    return dict(n_colors=len(used),
                names=sorted(name_of_index(i) for i in used),
                lum_min=lums[0] if lums else 0,
                lum_max=lums[-1] if lums else 0,
                lum_spread=spread)


# ---------------------------------------------------------------------------
# Composite review sheets
# ---------------------------------------------------------------------------

def review_sheet(sheet, out_path, area="graphics/area_rain.bin",
                 frames_to_show=None, title=None):
    """Build one PNG combining all four checks for a sheet."""
    from PIL import Image, ImageDraw

    n = sheet.frames if frames_to_show is None else min(frames_to_show,
                                                        sheet.frames)
    frames = [sheet.canvas(i) for i in range(n)]

    # Layout: rows of (color at 1x/2x/4x, silhouette at 1x, in-context at 1x/3x)
    s1, s2, s4 = 1, 2, 4
    cw1, ch1 = sheet.w * s1, sheet.h * s1
    cw2, ch2 = sheet.w * s2, sheet.h * s2
    cw4, ch4 = sheet.w * s4, sheet.h * s4
    gap = 8
    label_h = 12

    row_h = max(ch4, ch2) + label_h
    col_w = max(cw4, cw2 + cw1 + gap, cw1 * 3 + gap * 2)

    # in-context strip needs room for a slice of the area background
    ctx_w = sheet.w * 3 * 3          # three frames at 3x
    ctx_h = sheet.h * 3
    total_w = col_w + gap + max(ctx_w, cw1 * 4)
    total_h = row_h * n + 30

    bg = (46, 40, 58)
    out = Image.new("RGB", (total_w, total_h), bg)
    d = ImageDraw.Draw(out)
    if title:
        d.text((4, 4), title, fill=(255, 240, 200))

    # --- in-context background: a real slice of the area art ---
    ctx_bg = None
    if os.path.exists(area):
        raw = open(area, "rb").read()
        if len(raw) == 256 * 192:
            bimg = Image.new("RGB", (256, 192))
            bimg.putdata([idx_to_rgb(v) for v in raw])
            # crop a window roughly sprite-sized, scaled up
            crop = bimg.crop((0, 0, 64, 64)).resize(
                (ctx_w, ctx_h), Image.NEAREST)
            ctx_bg = crop

    for i, c in enumerate(frames):
        y = 30 + i * row_h
        x = 4

        # color at 4x
        img = Image.new("RGB", (sheet.w, sheet.h))
        img.putdata([idx_to_rgb(v) for v in c.px])
        out.paste(img.resize((cw4, ch4), Image.NEAREST), (x, y))

        # silhouette at 1x and 2x, stacked beside it
        sil = silhouette_canvas(c)
        sil1 = Image.new("RGB", (sheet.w, sheet.h), bg)
        sil1.paste(sil, (0, 0), sil)
        sx = x + cw4 + gap
        out.paste(sil1.resize((cw1, ch1), Image.NEAREST), (sx, y))
        out.paste(sil1.resize((cw2, ch2), Image.NEAREST), (sx + cw1 + 4, y))
        d.text((sx, y + ch2 + 1), "sil 1x / 2x", fill=(200, 200, 210))

        # color at 1x for scale honesty
        c1 = Image.new("RGB", (sheet.w, sheet.h), bg)
        c1.paste(img, (0, 0))
        px1 = sx + cw2 + gap
        out.paste(c1.resize((cw1, ch1), Image.NEAREST), (px1, y))
        d.text((px1, y + ch1 + 1), "1x", fill=(200, 200, 210))

        # in context at 3x
        if ctx_bg is not None:
            cxp = x + col_w + gap
            tile = ctx_bg.copy()
            big = img.resize((sheet.w * 3, sheet.h * 3), Image.NEAREST)
            mask = sil.resize((sheet.w * 3, sheet.h * 3), Image.NEAREST)
            tile.paste(big, (0, 0), mask)
            out.paste(tile, (cxp, y))

        d.text((x, y + ch4 + 1), f"frame {i} @4x", fill=(200, 200, 210))

    out.save(out_path)
    return out_path


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def analyse(sheet):
    """Print a text report for every frame of a sheet, and return failures."""
    spec_note = "" if sheet.matches_spec else f" -- spec says {sheet.frames}"
    print(f"\n=== {sheet.name} ({sheet.w}x{sheet.h}, "
          f"{sheet.actual_frames} frames{spec_note}) ===")
    problems = []
    if not sheet.matches_spec:
        problems.append(
            f"{sheet.name}: has {sheet.actual_frames} frames but the animation "
            f"spec requires {sheet.frames} -- not upgraded to the new spec yet")
    seen_first = None
    for i in range(sheet.actual_frames):
        c = sheet.canvas(i)
        sr = silhouette_report(c)
        pr = palette_report(c)

        if sr.get("empty"):
            problems.append(f"{sheet.name} frame {i}: completely empty")
            continue
        # Reaching the bottom edge is expected (the feet stand there) and the
        # shadow pass legitimately uses it; the sides and top are what clip.
        x0, y0, x1, y1 = sr["bbox"]
        side_clip = (x0 == 0 or x1 == sheet.w - 1)
        if y0 == 0:
            side_clip = True
        if side_clip:
            problems.append(
                f"{sheet.name} frame {i}: art touches the top or side edge "
                f"(bbox {sr['bbox']}) -- will look cropped against neighbours")
        if sr["fill_ratio"] < 0.12:
            problems.append(
                f"{sheet.name} frame {i}: only {sr['fill_ratio']:.0%} of the "
                f"frame is filled -- silhouette will read as thin noise")
        if pr["n_colors"] < 3:
            problems.append(
                f"{sheet.name} frame {i}: only {pr['n_colors']} color(s) -- "
                f"no light/shadow separation, will look flat")
        if pr["lum_spread"] < 60:
            problems.append(
                f"{sheet.name} frame {i}: value range is only "
                f"{pr['lum_spread']:.0f}/255 -- colors are too close in "
                f"brightness to show form")

        if i == 0:
            seen_first = (sr, pr)

    if seen_first:
        sr, pr = seen_first
        print(f"  frame 0: fill {sr['fill_ratio']:.0%}, "
              f"bbox {sr['bbox']}, {pr['n_colors']} colors, "
              f"luminance {pr['lum_min']:.0f}..{pr['lum_max']:.0f} "
              f"(spread {pr['lum_spread']:.0f})")
    return problems


def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)) + os.sep + "..")

    target = sys.argv[1] if len(sys.argv) > 1 else None
    sheets = discover_sheets()
    if target:
        sheets = [s for s in sheets if target in s.name]
    if not sheets:
        print("no matching sheets found")
        return 1

    outdir = "graphics/review"
    os.makedirs(outdir, exist_ok=True)

    all_problems = []
    for s in sheets:
        all_problems += analyse(s)
        # Pick a background the creature would actually be seen against, so the
        # review sheet shows the real contrast rather than an arbitrary ground.
        # Region names follow the ingredient each one grows.
        area = "graphics/area_rain.bin"
        if "mob_snow" in s.name or "icewolf" in s.name:
            area = "graphics/area_past.bin"
        elif "mob_scorp" in s.name or "sandworm" in s.name:
            area = "graphics/area_farm.bin"
        elif "ember" in s.name or "lava" in s.name:
            area = "graphics/area_cacao.bin"
        elif "slime" in s.name or "treant" in s.name:
            area = "graphics/area_marsh.bin"
        review_sheet(s, os.path.join(outdir, f"review_{s.name}.png"),
                     area=area, title=s.name)

    print(f"\nreview sheets -> {outdir}/")
    if all_problems:
        print(f"\n{len(all_problems)} issue(s) to fix:")
        for p in all_problems:
            print("  -", p)
    else:
        print("\nno structural issues found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
