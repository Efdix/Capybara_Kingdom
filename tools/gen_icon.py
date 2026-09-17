"""
ROM icon generator.

The icon is what shows up in the flashcart menu, so it is the first thing she
sees before launching the game -- worth drawing properly rather than shipping
the template placeholder. A DS icon is 32x32 GIF or PNG; we draw a birthday
cake with a candle and the two capybaras.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_common as gc
from gen_common import Canvas, ix


def build_icon():
    c = Canvas(32, 32)

    # Soft night-blue background with a warm glow behind the cake.
    c.rect(0, 0, 32, 32, ix("castle_dark"))
    c.ellipse(16, 20, 15, 12, ix("purple_dark"))
    c.ellipse(16, 21, 13, 9, ix("ui_panel_light"))
    for sx, sy in ((4, 3), (27, 5), (9, 7), (23, 2), (14, 4)):
        c.set(sx, sy, ix("white_spark"))

    # Cake: three layers, widest at the bottom.
    c.round_rect(3, 22, 26, 8, 2, ix("choco_dark"))
    c.round_rect(4, 23, 24, 6, 1, ix("choco_mid"))
    c.rect(6, 24, 20, 1, ix("choco_light"))

    c.round_rect(7, 16, 18, 7, 2, ix("sponge_dark"))
    c.round_rect(8, 17, 16, 5, 1, ix("sponge_mid"))
    c.rect(10, 18, 12, 1, ix("sponge_light"))

    c.round_rect(10, 11, 12, 6, 2, ix("cream_dark"))
    c.round_rect(11, 12, 10, 4, 1, ix("cream_mid"))

    # Frosting drips over the layer edges.
    for dx in range(5, 27, 4):
        c.round_rect(dx, 21, 3, 3, 1, ix("cream_mid"))
    for dx in range(9, 23, 4):
        c.round_rect(dx, 15, 3, 3, 1, ix("cream_mid"))

    # Cherry + candle with a lit flame.
    c.circle(16, 10, 3, ix("cherry_dark"))
    c.circle(16, 10, 2, ix("cherry_mid"))
    c.set(15, 9, ix("cherry_light"))
    c.rect(15, 3, 2, 6, ix("cyan_mid"))
    c.set(15, 3, ix("cyan_light"))
    c.circle(16, 2, 2, ix("ember_orange"))
    c.set(16, 1, ix("ember_yellow"))

    # The two capybaras peeking in from the bottom corners.
    for hx, variant in ((0, gc.VARIANT_MALE), (24, gc.VARIANT_FEMALE)):
        c.round_rect(hx + 1, 25, 7, 7, 2, ix("brn_mid", variant))
        c.round_rect(hx + 2, 26, 5, 3, 1, ix("brn_light", variant))
        for ex in (hx + 2, hx + 5):
            c.rect(ex, 27, 1, 1, ix("outline"))
        c.round_rect(hx + 2, 29, 5, 2, 1, ix("brn_belly", variant))
        c.round_rect(hx + 2, 23, 2, 3, 1, ix("brn_dark", variant))
        c.round_rect(hx + 5, 23, 2, 3, 1, ix("brn_dark", variant))

    c.auto_outline()
    return c


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "icon.gif"
    c = build_icon()

    # Save as both PNG (for inspection) and GIF (what ndstool accepts for the
    # ROM banner icon; GIF is the most broadly compatible choice for flashcarts).
    from PIL import Image

    def idx_to_rgb(i):
        for name in ["transparent"] + gc.PALETTE_NAMES:
            if gc.PAL[name] == i:
                rgb = gc.rgb_of(name)
                return rgb if name != "transparent" else (0, 0, 0)
        return (0, 0, 0)

    img = Image.new("RGB", (c.w, c.h))
    img.putdata([idx_to_rgb(v) for v in c.px])

    png = os.path.splitext(out)[0] + ".png"
    img.save(png)

    # A DS banner icon allows 16 palette entries, and ndstool reserves one of
    # them for transparency -- so the art itself may use only 15 colors. Left
    # to itself ndstool prints a warning and then writes a ROM with an empty
    # header, i.e. an unbootable file. Quantize to 15 explicitly and verify.
    quant = img.convert("P", palette=Image.ADAPTIVE, colors=15)
    if len(quant.getcolors() or []) > 15:
        raise SystemExit("icon still has more than 15 colors after quantizing")
    quant.save(out, format="GIF")

    # Confirm the file on disk really is a <=16 color GIF, since that is what
    # ndstool inspects.
    with Image.open(out) as chk:
        ncolors = len(chk.convert("P").getcolors() or [])
        if ncolors > 15:
            raise SystemExit(f"written icon has {ncolors} colors (>15)")
        if chk.size != (32, 32):
            raise SystemExit(f"icon is {chk.size}, expected 32x32")

    img.resize((c.w * 8, c.h * 8), Image.NEAREST).save(
        os.path.splitext(out)[0] + "_preview.png")
    print(f"icon -> {out} (and {png}), {ncolors} colors, 32x32")


if __name__ == "__main__":
    main()
