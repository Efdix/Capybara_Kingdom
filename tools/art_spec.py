"""
Art specification and creature layout for Capybara Kingdom.

Single source of truth for sprite geometry. The generators and the review tool
read it; the C engine mirrors it in assets.h. One source prevents the class of
bug where a generator changes size and the engine (or the reviewer) still assumes
the old one -- which already happened once here.

WHY SIZES ARE FREE

The DS hardware constrains hardware-sprite (OAM) dimensions to 8/16/32/64. This
game does not use hardware sprites: it composites a 256x192 8bpp bitmap in
software every frame, so sprite dimensions are not hardware-constrained at all.
Sizes are chosen for design reasons:

  swarm    16x16   many fast weak enemies, several spawned at once
  standard 32x32   the mainstay enemy
  elite    48x48   armoured, resists light hits
  boss     64x64   region bosses
  giant    96x96   the final boss, half the screen tall

Bigger creatures get fewer, slower frames. That is both a data saving and the
right feel: a giant should telegraph and lumber, a swarm should flicker.

LAYOUT IS DERIVED, NOT GUESSED

The first hero pass hardcoded part positions and clipped the canvas on 84 of 336
frames, then had to be patched frame by frame -- a slow way to converge and a
sign the approach was wrong. The layout here derives every part position from the
canvas size plus proportion fractions, and asserts the stack fits, so an
overflowing layout fails at generation time instead of shipping cropped art.
"""

# ---------------------------------------------------------------------------
# Size classes
# ---------------------------------------------------------------------------

HERO_W, HERO_H = 32, 32
SWARM_W, SWARM_H = 16, 16
STANDARD_W, STANDARD_H = 32, 32
ELITE_W, ELITE_H = 48, 48
BOSS_W, BOSS_H = 64, 64
GIANT_W, GIANT_H = 96, 96

# Every sprite reserves this border for the outline pass, so drawn art must stay
# inside it.
OUTLINE_MARGIN = 1


# ---------------------------------------------------------------------------
# Animation specs: (name, frames, loop)
# ---------------------------------------------------------------------------

# The hero is a quadruped: a real capybara shape -- long low body, big head
# slung forward, four short legs. There is no upright stance and no held
# weapon, so the attack is a bite and there is no separate "special"/"ultimate"
# pose. Every skill is drawn by the engine as its own effect (water jet, mud
# pool, dung pile, charge dust), which keeps the character sheet about the
# character.
HERO_ANIM = [
    ("idle",   4, True),
    ("walk",   6, True),
    ("attack", 4, False),   # the bite
    ("hurt",   2, False),
]

SWARM_ANIM = [
    ("idle",   2, True),
    ("move",   2, True),
    ("attack", 2, False),
    ("die",    3, False),
]

STANDARD_ANIM = [
    ("idle",   4, True),
    ("move",   4, True),
    ("attack", 4, False),
    ("hurt",   2, False),
    ("die",    5, False),
]

ELITE_ANIM = [
    ("idle",   4, True),
    ("move",   4, True),
    ("attack", 5, False),
    ("hurt",   2, False),
    ("die",    6, False),
]

BOSS_ANIM = [
    ("idle",      3, True),
    ("move",      3, True),
    ("telegraph", 3, False),   # wind-up the player must read and react to
    ("attack",    4, False),
    ("hurt",      2, False),
    ("die",       5, False),
]

GIANT_ANIM = [
    ("idle",      4, True),
    ("move",      4, True),
    ("telegraph", 4, False),
    ("attack",    5, False),
    ("hurt",      3, False),
    ("die",       8, False),
]


def frame_count(anim_spec):
    return sum(n for _name, n, _loop in anim_spec)


def frame_index(anim_spec, anim_name, offset=0):
    """Frame index for a named animation within one direction block."""
    base = 0
    for name, count, _loop in anim_spec:
        if name == anim_name:
            return base + (offset % count)
        base += count
    raise KeyError(anim_name)


HERO_DIRS = ["down", "up", "left", "right", "down_left", "down_right", "up_left", "up_right"]
HERO_FRAMES_PER_DIR = frame_count(HERO_ANIM)
HERO_TOTAL_FRAMES = HERO_FRAMES_PER_DIR * len(HERO_DIRS)



# ---------------------------------------------------------------------------
# Creature layout
# ---------------------------------------------------------------------------

class Layout:
    """Derived geometry for one creature in one canvas size.

    Coordinates are top-left based and all values are integers that fit inside
    the canvas with room for the outline.
    """

    def __init__(self, w, h, head_h, head_w, body_h, body_w, leg_h,
                 neck_h, ear_h, cx, top):
        self.w = w
        self.h = h
        self.head_h = head_h
        self.head_w = head_w
        self.body_h = body_h
        self.body_w = body_w
        self.leg_h = leg_h
        self.neck_h = neck_h
        self.ear_h = ear_h
        self.cx = cx

        self.ear_top = top
        self.head_top = top + ear_h
        self.neck_top = self.head_top + head_h
        self.body_top = self.neck_top + neck_h
        self.leg_top = self.body_top + body_h
        self.foot_y = self.leg_top + leg_h - 1

        # Where a held weapon attaches: at the body's upper right, roughly where
        # a raised paw would be.
        self.hand_x = cx + body_w // 2
        self.hand_y = self.body_top + 1

        # Widest half-extent, used by callers to check horizontal clearance.
        self.half_w = max(head_w, body_w) // 2

    def report(self):
        return (f"{self.w}x{self.h} cx={self.cx} "
                f"ear={self.ear_top} head={self.head_top}"
                f"..{self.head_top + self.head_h - 1} "
                f"body={self.body_top}"
                f"..{self.body_top + self.body_h - 1} "
                f"leg={self.leg_top}..{self.foot_y}")


def creature_layout(w, h, *, head_h_frac, body_h_frac, leg_h_frac,
                    head_w_frac, body_w_frac, neck_h=1, ear_h=2,
                    cx_frac=0.44, top_margin=None):
    """Derive part geometry from canvas size and proportion fractions.

    The fractions describe the creature's build: head-heavy reads as cute (or
    menacing, at small sizes), body-heavy reads as a lumbering brute.

    Asserts that the stack fits and that the creature clears the left margin.
    A layout that overflows must fail here rather than produce cropped art.
    """
    top = OUTLINE_MARGIN if top_margin is None else top_margin
    bottom_margin = OUTLINE_MARGIN
    avail = h - top - bottom_margin

    # Fractions are of the *usable* height, not the raw canvas: measuring against
    # the canvas left the creature floating in the top of its cell (the hero used
    # 24 of 32 rows, the giant 66 of 96), which wastes VRAM and breaks the
    # engine's foot-anchor math.
    #
    # The fractions are normalised so that the fixed parts (ears + neck) plus the
    # scaled parts exactly fill `avail`.
    fixed = ear_h + neck_h
    flexible = avail - fixed
    raw_head = head_h_frac
    raw_body = body_h_frac
    raw_leg = leg_h_frac
    raw_sum = raw_head + raw_body + raw_leg
    assert raw_sum > 0, "fractions must be positive"

    head_h = max(4, int(round(flexible * raw_head / raw_sum)))
    body_h = max(3, int(round(flexible * raw_body / raw_sum)))

    # Rounding can overshoot on small canvases, so pull the head back by the
    # overshoot: it keeps the proportion essentially identical and guarantees the
    # legs still get their minimum.
    overshoot = (head_h + body_h) - (flexible - 2)
    if overshoot > 0:
        head_h = max(4, head_h - overshoot)

    # Legs take the remainder, so the stack lands exactly on `avail` and the feet
    # sit on the last usable row.
    leg_h = max(2, flexible - head_h - body_h)

    head_w = max(6, int(round(w * head_w_frac)))
    body_w = max(5, int(round(w * body_w_frac)))

    total = fixed + head_h + body_h + leg_h
    assert total == avail, (
        f"layout does not fill the canvas: parts total {total} rows but {avail} "
        f"are available in a {w}x{h} canvas")

    # The outline pass then adds one row below the feet, landing on row h-1,
    # which is still inside the canvas. Reaching h-1 is therefore expected and
    # correct; only art drawn *on* row h-1 before outlining would clip.
    assert leg_h + fixed + head_h + body_h + top + OUTLINE_MARGIN <= h, (
        f"layout plus its outline overflows the {w}x{h} canvas")

    cx = int(round(w * cx_frac))
    half = max(head_w, body_w) // 2
    assert cx - half >= OUTLINE_MARGIN, (
        f"creature centre x={cx} with half-width {half} crosses the left margin "
        f"of a {w}x{w} canvas")

    return Layout(w, h, head_h, head_w, body_h, body_w, leg_h, neck_h, ear_h,
                  cx, top)


# Ready-made layouts.
#
# The hero keeps a large head relative to the body -- that proportion is what
# makes a capybara read as a capybara. The giant inverts it, so it reads as heavy
# rather than cute.
class QuadLayout:
    """Geometry for a four-legged animal, derived from the canvas size.

    This replaces the earlier head/neck/body/leg stack. That stack described an
    upright biped: a neck under the head, then a torso, then legs hanging from
    its bottom. A capybara is built the other way round -- the head sits at the
    FRONT of a long horizontal barrel and all four legs hang from that barrel's
    underside, front pair under the chest and rear pair under the haunches.
    The body is therefore measured horizontally, which is why the old vertical
    fractions could not simply be retuned.

    Vertical stack (top to bottom), filling the usable rows exactly:

        ear band     small, the ear nubs poke above the skull
        torso band   one band shared by head and barrel -- they overlap
                     vertically, so the band is the TALLER of the two
        leg band     all four legs

    Horizontally, the head takes the front (right, before mirroring) end and the
    barrel the rear, and the two overlap by `neck_overlap` columns so the join
    reads as a shoulder rather than as a seam between two pasted shapes.

    Side view ("left"/"right"): head forward, barrel behind -- the shape that
    makes the species readable.
    Front/back view ("down"/"up"): the animal is seen head-on, so the head is
    centred over the barrel instead of offset to the front.
    """

    def __init__(self, w, h, *, head_h, head_w, body_h, body_w, leg_h,
                 ear_h):
        self.w = w
        self.h = h
        self.head_h = head_h
        self.head_w = head_w
        self.body_h = body_h
        self.body_w = body_w
        self.leg_h = leg_h
        self.ear_h = ear_h

        # Bottom-anchored: the feet land on the last usable row. The engine
        # blits sprites with a fixed downward shadow offset and treats the foot
        # row as the ground contact, so a floating animal would hover above the
        # terrain and its shadow would land on its own belly.
        band = max(head_h, body_h)
        self.foot_y = h - OUTLINE_MARGIN - 1
        self.leg_top = self.foot_y - leg_h + 1
        self.torso_bottom = self.leg_top - 1
        self.torso_top = self.torso_bottom - band + 1
        self.ear_top = self.torso_top - ear_h
        assert self.ear_top >= OUTLINE_MARGIN, (
            f"quadruped overflows the top: ears start at row {self.ear_top}, "
            f"margin is {OUTLINE_MARGIN}")
        # Air left above the back. Non-zero is expected and correct for a low,
        # wide animal in a square cell; a negative value would mean clipping.
        self.headroom = self.ear_top - OUTLINE_MARGIN

        # The barrel spans the whole band; the head is shorter and sits low in
        # it, the way a capybara's muzzle hangs below its eye line while the
        # skull top stays near the back line.
        self.head_dy = band - head_h

        self.body_top = self.torso_top
        self.body_bottom = self.torso_bottom
        self.head_top = self.torso_top + self.head_dy
        self.head_bottom = self.head_top + head_h - 1

        # Side view: head at the front end, plus a blunt snout that protrudes
        # past the skull. That protruding muzzle is the single strongest
        # capybara tell -- a head that stops at the skull reads as a generic
        # bear or guinea pig -- so the layout reserves columns for it rather
        # than letting the painter draw off the edge.
        self.neck_overlap = max(2, head_w // 3)
        self.snout_len = max(1, int(round(w * 0.07)))
        span = head_w + body_w - self.neck_overlap
        left = max(OUTLINE_MARGIN,
                   (w - span - self.snout_len) // 2)
        self.body_left = left
        self.body_right = left + body_w - 1
        self.head_left = self.body_right - self.neck_overlap + 1
        self.head_right = self.head_left + head_w - 1
        self.snout_right = self.head_right + self.snout_len
        assert self.snout_right <= w - OUTLINE_MARGIN, (
            f"snout reaches column {self.snout_right} in a {w}-wide canvas")

        self.head_top = self.torso_top + self.head_dy
        # Muzzle tip: where the bite effect and the facing read come from.
        self.muzzle_x = self.snout_right
        self.muzzle_y = self.head_top + head_h - 2
        self.cx = (self.body_left + self.snout_right) // 2
        self.half_w = max(self.snout_right - self.cx, self.cx - self.body_left)

        # Front/back view: head centred over the barrel.
        self.front_head_left = self.cx - head_w // 2
        self.front_body_left = self.cx - body_w // 2

    def report(self):
        return (f"{self.w}x{self.h} ear={self.ear_top} "
                f"torso={self.torso_top}..{self.torso_bottom} "
                f"leg={self.leg_top}..{self.foot_y} "
                f"body_x={self.body_left}..{self.body_right} "
                f"head_x={self.head_left}..{self.head_right} "
                f"snout_x=..{self.snout_right}")


def quadruped_layout(w, h, *, head_h_frac, head_w_frac, body_h_frac,
                     body_w_frac, leg_h_frac, ear_h=None):
    """Derive a four-legged animal's geometry from canvas size + fractions.

    Fractions are of the usable height (canvas minus the outline margins). The
    legs and ears take their share off the bottom and top, and the head/body
    band is whatever is left; head_h_frac and body_h_frac are fractions *of that
    band*, so they describe the two shapes against each other rather than
    against the whole canvas. Both are bottom-aligned to the band, so the head
    (normally the taller of the two) rises above the back -- the capybara
    silhouette.

    Asserts both that the vertical stack fits and that head + barrel fit side by
    side. A layout that overflows must fail here rather than ship cropped art --
    the failure mode the previous biped layout existed to prevent, and just as
    easy to hit with a new shape.
    """
    avail = h - OUTLINE_MARGIN * 2

    ear_h = max(1, int(round(avail * 0.07))) if ear_h is None else ear_h
    flexible = avail - ear_h

    leg_h = max(2, int(round(flexible * leg_h_frac)))
    band = flexible - leg_h
    assert band >= 6, (
        f"only {band} rows left for the head and body in a {w}x{h} canvas")

    head_h = min(band, max(4, int(round(band * head_h_frac))))
    body_h = min(band, max(3, int(round(band * body_h_frac))))

    head_w = max(6, int(round(w * head_w_frac)))
    body_w = max(5, int(round(w * body_w_frac)))

    inner = w - OUTLINE_MARGIN * 2
    overlap = max(2, head_w // 3)
    snout_len = max(1, int(round(w * 0.07)))
    span = head_w + body_w - overlap + snout_len
    assert span <= inner, (
        f"quadruped does not fit horizontally: head {head_w} + snout "
        f"{snout_len} + body {body_w} - overlap {overlap} = {span} exceeds "
        f"{inner} usable columns")

    return QuadLayout(w, h, head_h=head_h, head_w=head_w, body_h=body_h,
                      body_w=body_w, leg_h=leg_h, ear_h=ear_h)


def hero_layout(w=HERO_W, h=HERO_H):
    """A capybara: low barrel body, big boxy head forward, four stubby legs."""
    return quadruped_layout(
        w, h,
        head_h_frac=0.64, head_w_frac=0.42,   # head 13w x 14h of a 32 canvas
        body_h_frac=0.55, body_w_frac=0.50,   # barrel 16w x 12h
        leg_h_frac=0.21,
    )


def swarm_layout():
    return quadruped_layout(
        SWARM_W, SWARM_H,
        head_h_frac=0.68, head_w_frac=0.50,
        body_h_frac=0.55, body_w_frac=0.46,
        leg_h_frac=0.24,
    )


def standard_layout():
    return quadruped_layout(
        STANDARD_W, STANDARD_H,
        head_h_frac=0.66, head_w_frac=0.42,
        body_h_frac=0.55, body_w_frac=0.50,
        leg_h_frac=0.21,
    )


def elite_layout():
    return quadruped_layout(
        ELITE_W, ELITE_H,
        head_h_frac=0.62, head_w_frac=0.36,
        body_h_frac=0.58, body_w_frac=0.52,
        leg_h_frac=0.20,
    )


def boss_layout():
    return quadruped_layout(
        BOSS_W, BOSS_H,
        head_h_frac=0.60, head_w_frac=0.34,
        body_h_frac=0.58, body_w_frac=0.54,
        leg_h_frac=0.20,
    )


def giant_layout():
    return quadruped_layout(
        GIANT_W, GIANT_H,
        head_h_frac=0.56, head_w_frac=0.30,
        body_h_frac=0.60, body_w_frac=0.56,
        leg_h_frac=0.20,
    )


# ---------------------------------------------------------------------------
# Quality thresholds used by art_review.py
# ---------------------------------------------------------------------------

# A sprite whose colors all sit within a narrow brightness band reads as a flat
# sticker: there is no light/shadow separation, so the eye cannot parse volume.
MIN_LUM_SPREAD = 60

# Minimum distinct colors for the form to read at a given size.
MIN_COLORS = {
    (16, 16): 6,
    (24, 24): 8,
    (32, 32): 10,
    (48, 48): 14,
    (64, 64): 16,
    (96, 96): 20,
}


def min_colors_for(w, h):
    return MIN_COLORS.get((w, h), max(6, (w * h) // 100))


# ---------------------------------------------------------------------------
# Self-check: assert every layout fits before anything is generated.
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    for name, fn in (("hero", hero_layout), ("swarm", swarm_layout),
                     ("standard", standard_layout), ("elite", elite_layout),
                     ("boss", boss_layout), ("giant", giant_layout)):
        lay = fn()
        print(f"{name:9s} {lay.report()}")
    print("\nall layouts fit")
