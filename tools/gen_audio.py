#!/usr/bin/env python3
"""
gen_audio.py -- procedural music + sound-effect generator for "Capybara Kingdom"
(a Nintendo DS homebrew game built with BlocksDS + maxmod).

Usage:
    python gen_audio.py [output_root]        # default output_root = "audio"

Outputs (all generated from scratch, standard library only, no numpy):

    <root>/sfx/<name>.wav      16 sound effects, 16-bit signed mono PCM @ 22050 Hz
    <root>/music/<name>.mod     6 ProTracker modules (4 channels, 8-bit samples)
    <root>/preview_waveforms.png  verification render of birthday.mod

File naming matters: mmutil turns the *file name* into the C identifier used by
the game, so "sfx/ui_move.wav" becomes SFX_UI_MOVE and "music/title.mod"
becomes MOD_TITLE (upper-cased, non-alphanumerics folded to '_').

The MOD writer below emits the classical 31-instrument ProTracker layout
(M.K.) by hand; the preview renderer at the bottom reads those files back and
replays them with maxmod's own pitch rule, so the PNG is a picture of the
actual module bytes, not of a separate note list.

Pitch reference used by maxmod's MOD player (verified against note_table_mod /
mmGetPeriod in libmm7.a):

    frequency_hz = 8363 * 1712 / period

so a sample holding exactly N full cycles in S bytes plays N*8363*1712/(S*period)
Hz.  The lead/bass waveforms here are one and two cycles of 128 samples each,
which makes every period in the table land on the intended tempered pitch
(period 428 -> C4 = 261 Hz, period 214 -> C5 = 523 Hz, ...).
"""

import math
import os
import random
import struct
import sys
import wave

# --------------------------------------------------------------------------
# global constants
# --------------------------------------------------------------------------

TAU = 2.0 * math.pi
SFX_RATE = 22050                       # Hz, required output rate for SFX
FULL_SCALE = 32767                     # 16-bit signed full scale
MOD_BASE_RATE = 8363.0                 # Hz, "middle C" sample rate in maxmod
MOD_PERIOD_C = 1712.0                  # period that plays a sample at MOD_BASE_RATE
PREVIEW_RATE = 32768                   # maxmod's DS mixer rate, used for the PNG

LEAD, BASS, KICK, SNARE, HAT, PAD = 1, 2, 3, 4, 5, 6   # sample slots per song

TICK_RATE = 50.0                       # Amiga/ProTracker ticks per second


# --------------------------------------------------------------------------
# generic dsp helpers
# --------------------------------------------------------------------------

def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def norm_peak(buf, peak=1.0):
    """Scale a float buffer so its maximum magnitude is exactly `peak`."""
    m = 0.0
    for v in buf:
        a = -v if v < 0.0 else v
        if a > m:
            m = a
    if m < 1e-12:
        raise ValueError("refusing to normalize a silent buffer")
    g = peak / m
    return [v * g for v in buf]


def noise(n, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


def lowpass(x, cutoff, sr):
    """One-pole low pass. `cutoff` is a Hz scalar or a per-sample list."""
    y = [0.0] * len(x)
    prev = 0.0
    if isinstance(cutoff, (int, float)):
        k = 1.0 - math.exp(-TAU * max(float(cutoff), 1.0) / sr)
        for i, v in enumerate(x):
            prev += k * (v - prev)
            y[i] = prev
    else:
        for i, v in enumerate(x):
            k = 1.0 - math.exp(-TAU * max(cutoff[i], 1.0) / sr)
            prev += k * (v - prev)
            y[i] = prev
    return y


def highpass(x, a=0.93):
    """One-pole high pass; `a` near 1.0 = gentle, lower = brighter."""
    y = [0.0] * len(x)
    px = 0.0
    py = 0.0
    for i, v in enumerate(x):
        py = a * (py + v - px)
        px = v
        y[i] = py
    return y


def differentiate(x, times=1):
    for _ in range(times):
        out = [0.0] * len(x)
        prev = 0.0
        for i, v in enumerate(x):
            out[i] = v - prev
            prev = v
        x = out
    return x


def env_ar(n, sr, attack=0.002, tau=0.05, power=1.0):
    """Linear attack followed by an exponential decay (time constant `tau`)."""
    a = max(1, int(attack * sr))
    out = [0.0] * n
    for i in range(n):
        if i < a:
            e = i / a
        else:
            e = math.exp(-(((i - a) / (tau * sr)) ** power))
        out[i] = e
    return out


def env_piecewise(n, points):
    """`points` = [(fraction_of_length, value), ...] sorted by fraction."""
    out = [0.0] * n
    for i in range(n):
        f = i / max(1, n - 1)
        prev_f, prev_v = points[0]
        if f <= prev_f:
            out[i] = prev_v
            continue
        for j in range(1, len(points)):
            nf, nv = points[j]
            if f <= nf:
                t = (f - prev_f) / max(1e-9, nf - prev_f)
                out[i] = prev_v + (nv - prev_v) * t
                break
            prev_f, prev_v = nf, nv
        else:
            out[i] = points[-1][1]
    return out


def add_into(dst, src, offset=0, gain=1.0):
    end = min(len(dst), offset + len(src))
    for i in range(offset, end):
        dst[i] += src[i - offset] * gain
    return dst


def mul_env(x, env):
    return [a * b for a, b in zip(x, env)]


def add_synth(n, sr, freq_fn, partials, attack=0.0):
    """Additive oscillator with a per-sample frequency function.

    `partials` is a list of (ratio, amplitude) pairs; per-partial phase is
    accumulated so the waveform stays continuous through frequency sweeps,
    which keeps chirps free of clicks.
    """
    out = [0.0] * n
    phases = [0.0] * len(partials)
    amp_env = env_ar(n, sr, attack=attack if attack > 0 else 1.0 / sr, tau=1e9)
    for i in range(n):
        f = freq_fn(i)
        s = 0.0
        for j, (ratio, amp) in enumerate(partials):
            phases[j] += TAU * ratio * f / sr
            s += amp * math.sin(phases[j])
        out[i] = s * amp_env[i]
    return out


# Band-limited "chip" spectra.  Harmonics are kept low enough that the highest
# tone we ever use (B6 / 1975 Hz) stays under Nyquist at 22050 Hz.
SOFT_SQUARE = [(1, 1.00), (3, 0.34), (5, 0.18), (7, 0.10)]
BRIGHT_SQUARE = [(1, 1.00), (3, 0.44), (5, 0.28), (7, 0.18), (9, 0.10)]
TRIANGLE = [(1, 1.00), (3, -0.11), (5, 0.04), (7, -0.02)]
WARM_SINE = [(1, 1.00), (2, 0.18), (3, 0.05)]
BELL = [(1, 1.00), (2.76, 0.34), (5.40, 0.13), (8.93, 0.05)]


# --------------------------------------------------------------------------
# WAV output
# --------------------------------------------------------------------------

def write_wav(path, floats, peak, sr=SFX_RATE):
    """Normalize to `peak` (fraction of full scale) and write 16-bit mono PCM."""
    buf = norm_peak(floats, peak)
    frames = bytearray()
    for v in buf:
        s = int(round(v * FULL_SCALE))
        s = clamp(s, -FULL_SCALE, FULL_SCALE)
        frames += struct.pack("<h", s)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(bytes(frames))
    return len(buf)


# --------------------------------------------------------------------------
# the 16 sound effects
# --------------------------------------------------------------------------

def nsamp(seconds):
    """Sample count for a duration, rounded up so lengths never fall short."""
    return int(math.ceil(seconds * SFX_RATE))


def sfx_ui_move(rng):
    """Very short soft blip, 1200 Hz square-ish."""
    n = nsamp(0.05)
    t = add_synth(n, SFX_RATE, lambda i: 1200.0, SOFT_SQUARE, attack=0.0006)
    return mul_env(t, env_ar(n, SFX_RATE, attack=0.0006, tau=0.014))


def sfx_two_tone(rng, f_lo, f_hi):
    """Two stacked blip segments (rising when f_hi > f_lo, falling otherwise)."""
    seg = int(0.06 * SFX_RATE)
    step = int(0.045 * SFX_RATE)
    total = step + seg
    out = [0.0] * total
    for k, f in enumerate((f_lo, f_hi)):
        a = add_synth(seg, SFX_RATE, lambda i, f=f: f, SOFT_SQUARE, attack=0.001)
        a = mul_env(a, env_ar(seg, SFX_RATE, attack=0.001, tau=0.028))
        add_into(out, a, k * step, 1.0 if k == 0 else 0.95)
    return out


def sfx_attack(rng):
    """Short noise whoosh with a descending filter, 0.12 s."""
    n = int(0.12 * SFX_RATE)
    src = noise(n, rng)
    cutoff = [6000.0 * math.exp(-i / (0.045 * SFX_RATE)) + 500.0 for i in range(n)]
    body = lowpass(src, cutoff, SFX_RATE)
    body = lowpass(body, cutoff, SFX_RATE)          # two poles = steeper sweep
    env = env_piecewise(n, [(0.0, 0.0), (0.18, 1.0), (0.55, 0.45), (1.0, 0.0)])
    return mul_env(body, env)


def sfx_hit(rng):
    """Bright noise burst + low thud, 0.1 s."""
    n = int(0.1 * SFX_RATE)
    crack = differentiate(highpass(noise(n, rng), 0.86), 1)
    crack = mul_env(crack, env_ar(n, SFX_RATE, attack=0.0004, tau=0.012))
    thud = add_synth(n, SFX_RATE,
                     lambda i: 55.0 + 45.0 * math.exp(-i / (0.02 * SFX_RATE)),
                     [(1, 1.0), (2, 0.15)], attack=0.001)
    thud = mul_env(thud, env_ar(n, SFX_RATE, attack=0.001, tau=0.035))
    return [1.0 * c + 0.85 * t for c, t in zip(crack, thud)]


def sfx_hurt(rng):
    """Descending square chirp 600 -> 220 Hz, 0.2 s."""
    n = int(0.2 * SFX_RATE)
    f = lambda i: 220.0 + 380.0 * math.exp(-i / (0.075 * SFX_RATE))
    t = add_synth(n, SFX_RATE, f, SOFT_SQUARE + [(2, 0.06)], attack=0.003)
    return mul_env(t, env_ar(n, SFX_RATE, attack=0.003, tau=0.095))


def sfx_enemy_die(rng):
    """Noise burst with a falling pitch, 0.25 s."""
    n = int(0.25 * SFX_RATE)
    src = noise(n, rng)
    cutoff = [5000.0 * math.exp(-i / (0.07 * SFX_RATE)) + 300.0 for i in range(n)]
    body = lowpass(lowpass(src, cutoff, SFX_RATE), cutoff, SFX_RATE)
    body = mul_env(body, env_ar(n, SFX_RATE, attack=0.002, tau=0.065))
    f = lambda i: 90.0 + 330.0 * math.exp(-i / (0.06 * SFX_RATE))
    tone = add_synth(n, SFX_RATE, f, BRIGHT_SQUARE, attack=0.002)
    tone = mul_env(tone, env_ar(n, SFX_RATE, attack=0.002, tau=0.05))
    return [0.9 * b + 0.7 * t for b, t in zip(body, tone)]


def sfx_magic_fire(rng):
    """Filtered noise swell with crackle, 0.35 s."""
    n = int(0.35 * SFX_RATE)
    src = noise(n, rng)
    cutoff = [400.0 + 2600.0 * math.sin(math.pi * i / n) for i in range(n)]
    body = lowpass(src, cutoff, SFX_RATE)
    env = env_piecewise(n, [(0.0, 0.0), (0.14, 0.9), (0.4, 1.0), (1.0, 0.0)])
    body = mul_env(body, env)
    # sparse crackle: tiny noise pops sprinkled through the swell
    for _ in range(46):
        pos = rng.randrange(0, max(1, n - 40))
        ln = rng.randrange(6, 34)
        g = rng.uniform(0.25, 1.0) * (1.0 - pos / n)
        pop = mul_env(noise(ln, rng), env_ar(ln, SFX_RATE, attack=0.0004, tau=0.004))
        add_into(body, pop, pos, g)
    return body


def sfx_magic_ice(rng):
    """High glassy detuned sines with fast tremolo, 0.35 s."""
    n = int(0.35 * SFX_RATE)
    out = [0.0] * n
    base = [1568.0, 2093.0, 2637.0, 3136.0, 3951.0, 4699.0]
    for k, f0 in enumerate(base):
        det = 1.0 + rng.uniform(-0.013, 0.013)
        f = f0 * det
        tone = add_synth(n, SFX_RATE, lambda i, f=f: f,
                         [(1, 1.0), (2, 0.07)], attack=0.008)
        trem_hz = 17.0 + 4.0 * k + rng.uniform(-1.0, 1.0)
        trem = [0.60 + 0.40 * math.sin(TAU * trem_hz * i / SFX_RATE + k)
                for i in range(n)]
        dec = env_ar(n, SFX_RATE, attack=0.008, tau=0.20 + 0.03 * k)
        g = 0.85 ** k
        add_into(out, [t * m * d * g for t, m, d in zip(tone, trem, dec)])
    return out


def sfx_magic_thunder(rng):
    """Sharp crack + long noise rumble, 0.4 s."""
    n = int(0.4 * SFX_RATE)
    crack = differentiate(highpass(noise(n, rng), 0.9), 2)
    crack = mul_env(crack, env_ar(n, SFX_RATE, attack=0.0003, tau=0.007))
    src = noise(n, rng)
    cutoff = [320.0 * math.exp(-i / (0.22 * SFX_RATE)) + 110.0 for i in range(n)]
    rumble = lowpass(lowpass(src, cutoff, SFX_RATE), cutoff, SFX_RATE)
    rumble = mul_env(rumble, env_ar(n, SFX_RATE, attack=0.006, tau=0.17))
    sub = add_synth(n, SFX_RATE,
                    lambda i: 38.0 + 34.0 * math.exp(-i / (0.05 * SFX_RATE)),
                    [(1, 1.0)], attack=0.002)
    sub = mul_env(sub, env_ar(n, SFX_RATE, attack=0.002, tau=0.10))
    return [1.0 * c + 0.85 * r + 0.6 * s for c, r, s in zip(crack, rumble, sub)]


def sfx_heal(rng):
    """Gentle rising arpeggio of soft sine tones: C5 E5 G5 C6, 0.4 s."""
    notes = [523.25, 659.26, 783.99, 1046.50]
    seg = int(0.13 * SFX_RATE)
    step = int(0.095 * SFX_RATE)
    total = step * (len(notes) - 1) + seg
    out = [0.0] * total
    for k, f in enumerate(notes):
        t = add_synth(seg, SFX_RATE, lambda i, f=f: f,
                      [(1, 1.0), (2, 0.13), (3, 0.04)], attack=0.010)
        t = mul_env(t, env_ar(seg, SFX_RATE, attack=0.010, tau=0.075))
        add_into(out, t, k * step, 0.85 + 0.05 * k)
    return out


def sfx_pickup(rng):
    """Bright ascending two-note chime, 0.15 s."""
    notes = [1046.50, 1567.98]
    seg = int(0.085 * SFX_RATE)
    step = int(0.062 * SFX_RATE)
    total = step + seg
    out = [0.0] * total
    for k, f in enumerate(notes):
        t = add_synth(seg, SFX_RATE, lambda i, f=f: f, BELL, attack=0.0012)
        t = mul_env(t, env_ar(seg, SFX_RATE, attack=0.0012, tau=0.030))
        add_into(out, t, k * step, 1.0 if k == 0 else 0.9)
    return out


def sfx_levelup(rng):
    """Four-note ascending fanfare C5 E5 G5 C6, 0.5 s, square + triangle."""
    notes = [523.25, 659.26, 783.99, 1046.50]
    seg = int(0.16 * SFX_RATE)
    step = int(0.105 * SFX_RATE)
    total = step * (len(notes) - 1) + seg
    out = [0.0] * total
    for k, f in enumerate(notes):
        sq = add_synth(seg, SFX_RATE, lambda i, f=f: f, SOFT_SQUARE, attack=0.006)
        tri = add_synth(seg, SFX_RATE, lambda i, f=f: f, TRIANGLE, attack=0.006)
        t = [0.6 * a + 0.85 * b for a, b in zip(sq, tri)]
        tau = 0.13 if k < len(notes) - 1 else 0.24
        t = mul_env(t, env_ar(seg, SFX_RATE, attack=0.006, tau=tau))
        add_into(out, t, k * step, 0.8 + 0.07 * k)
    return out


def sfx_door(rng):
    """Low rumble + creak, 0.4 s."""
    n = int(0.4 * SFX_RATE)
    src = noise(n, rng)
    rumble = lowpass(lowpass(src, 150.0, SFX_RATE), 150.0, SFX_RATE)
    am = [0.55 + 0.45 * math.sin(TAU * 3.3 * i / SFX_RATE) for i in range(n)]
    rumble = mul_env(rumble, [a * b for a, b in
                              zip(am, env_ar(n, SFX_RATE, attack=0.02, tau=0.22))])
    wob = lambda i: 520.0 * (1.0 + 0.09 * math.sin(TAU * 1.7 * i / SFX_RATE)) \
        + 60.0 * math.sin(TAU * 0.9 * i / SFX_RATE)
    creak = add_synth(n, SFX_RATE, wob, [(1, 1.0), (2, 0.30), (3, 0.14)], attack=0.012)
    irr = env_piecewise(n, [(0.0, 0.1), (0.2, 1.0), (0.35, 0.55), (0.5, 0.95),
                            (0.7, 0.4), (1.0, 0.0)])
    creak = mul_env(creak, [a * b for a, b in
                            zip(irr, env_ar(n, SFX_RATE, attack=0.012, tau=0.14))])
    band = lowpass(noise(n, rng), 900.0, SFX_RATE)
    band = [a - b for a, b in zip(band, lowpass(band, 420.0, SFX_RATE))]
    band = mul_env(band, env_ar(n, SFX_RATE, attack=0.02, tau=0.12))
    return [0.9 * r + 0.55 * c + 0.35 * b for r, c, b in zip(rumble, creak, band)]


def sfx_explosion(rng):
    """Big noise burst with a long decay, 0.6 s."""
    n = int(0.6 * SFX_RATE)
    src = noise(n, rng)
    cutoff = [7500.0 * math.exp(-i / (0.09 * SFX_RATE)) + 170.0 for i in range(n)]
    body = lowpass(lowpass(src, cutoff, SFX_RATE), cutoff, SFX_RATE)
    body = mul_env(body, env_ar(n, SFX_RATE, attack=0.003, tau=0.155))
    trans = mul_env(noise(n, rng), env_ar(n, SFX_RATE, attack=0.0003, tau=0.005))
    thump = add_synth(n, SFX_RATE,
                      lambda i: 30.0 + 40.0 * math.exp(-i / (0.05 * SFX_RATE)),
                      [(1, 1.0), (2, 0.12)], attack=0.002)
    thump = mul_env(thump, env_ar(n, SFX_RATE, attack=0.002, tau=0.11))
    return [1.0 * b + 0.8 * t + 0.75 * p for b, t, p in zip(body, trans, thump)]


def sfx_heart(rng):
    """Soft warm two-note blip, 0.12 s."""
    notes = [659.26, 880.00]
    seg = int(0.068 * SFX_RATE)
    step = int(0.05 * SFX_RATE)
    total = step + seg
    out = [0.0] * total
    for k, f in enumerate(notes):
        t = add_synth(seg, SFX_RATE, lambda i, f=f: f,
                      [(1, 1.0), (2, 0.20), (3, 0.06)], attack=0.004)
        t = mul_env(t, env_ar(seg, SFX_RATE, attack=0.004, tau=0.026))
        add_into(out, t, k * step, 1.0 if k == 0 else 0.95)
    return out


# name -> (generator, target peak as a fraction of full scale)
SFX_SPECS = [
    ("ui_move",       sfx_ui_move,        0.74),
    ("ui_confirm",    lambda r: sfx_two_tone(r, 880.0, 1320.0),  0.76),
    ("ui_cancel",     lambda r: sfx_two_tone(r, 1250.0, 700.0),  0.76),
    ("attack",        sfx_attack,         0.80),
    ("hit",           sfx_hit,            0.82),
    ("hurt",          sfx_hurt,           0.78),
    ("enemy_die",     sfx_enemy_die,      0.80),
    ("magic_fire",    sfx_magic_fire,     0.78),
    ("magic_ice",     sfx_magic_ice,      0.74),
    ("magic_thunder", sfx_magic_thunder,  0.84),
    ("heal",          sfx_heal,           0.72),
    ("pickup",        sfx_pickup,         0.76),
    ("levelup",       sfx_levelup,        0.78),
    ("door",          sfx_door,           0.78),
    ("explosion",     sfx_explosion,      0.85),
    ("heart",         sfx_heart,          0.72),
]


# --------------------------------------------------------------------------
# ProTracker period table + note parsing
# --------------------------------------------------------------------------
# Layout: 12 notes (C..B) per row, row 0 = C2 .. row 4 = B6 in scientific
# pitch notation.  Rows 1..3 are the canonical octaves the brief asks for
# (C-1 = 856 ... C-3 = 214 in ProTracker naming, which is scientific C3..C5);
# row 0 (periods 1712..906) exists because maxmod happily plays down to
# period 1712, which lets the bass lines sit at a real bass register (C2 = 65
# Hz) instead of one octave up.
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

PERIODS = [
    1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 906,  # sci 2
    856, 808, 762, 720, 678, 640, 604, 570, 538, 509, 480, 453,            # sci 3
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,            # sci 4
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,            # sci 5
    107, 101, 95, 90, 85, 80, 76, 71, 67, 64, 60, 57,                      # sci 6
    54, 51, 48, 45, 42, 40, 38, 36, 34, 32, 30, 28,                        # sci 7
]


def sci_to_period(name):
    """'G4'/'F#5'/'Bb2'/'C-4' -> ProTracker period, transposed into range.

    Scientific octave N maps to table row N-2 (scientific C4 = middle C = the
    ProTracker C-2 = period 428).  Notes outside the table are transposed by
    whole octaves, down when they are too high and up when they are too low.
    """
    s = name.strip()
    letter = s[0].upper()
    rest = s[1:]
    acc = 0
    if rest[:1] in ("#", "b", "-"):
        if rest[0] == "#":
            acc = 1
        elif rest[0] == "b":
            acc = -1
        rest = rest[1:]
    if not rest:
        raise ValueError("note %r has no octave" % name)
    octave = int(rest)
    idx = (octave - 2) * 12 + NOTE_NAMES.index(letter) + acc
    while idx < 0:
        idx += 12
    while idx >= len(PERIODS):
        idx -= 12
    return PERIODS[idx]


# --------------------------------------------------------------------------
# MOD (ProTracker, M.K.) writer
# --------------------------------------------------------------------------

def mod_cell(sample, period, effect=0, param=0):
    """Pack one 4-byte ProTracker pattern cell.

    The sample number is 5 bits and is split across two bytes; getting this
    wrong silently drops the low nibble, which turns sample 1..15 into 0
    (i.e. no sound at all). Verified against a reference M.K. module:

        byte0 = (sample_hi << 4) | (period_hi)      sample_hi = sample >> 4
        byte1 =  period & 0xFF
        byte2 = (sample_lo << 4) | (effect & 0x0F)  sample_lo = sample & 0x0F
        byte3 =  param & 0xFF
    """
    if period is not None and not (0 <= period < 4096):
        raise ValueError("period %r out of range" % period)
    p = period or 0
    return bytes(((sample & 0xF0) | ((p >> 8) & 0x0F),
                  p & 0xFF,
                  ((sample & 0x0F) << 4) | (effect & 0x0F),
                  param & 0xFF))


def mod_cell_decode(cell):
    """Inverse of mod_cell: (sample, period, effect, param)."""
    b0, b1, b2, b3 = cell
    return ((b0 & 0xF0) | (b2 >> 4),
            ((b0 & 0x0F) << 8) | b1,
            b2 & 0x0F,
            b3)


class Pattern(object):
    """64 rows x 4 channels of MOD cells; empty cells are 00 00 00 00."""

    ROWS = 64
    CHANNELS = 4

    def __init__(self):
        self.cells = [dict() for _ in range(self.CHANNELS)]

    def put(self, ch, row, sample, note=None, period=None, effect=0, param=0):
        if note is not None:
            period = sci_to_period(note)
        self.cells[ch][row] = mod_cell(sample, period, effect, param)

    def notes(self, ch, sample, events):
        """events: iterable of (row, note)."""
        for row, note in events:
            self.put(ch, row, sample, note=note)

    def events_of(self, ch):
        """[(row, sample, period)] sorted by row, for rendering/verification."""
        out = []
        for r in self.cells[ch]:
            cell = self.cells[ch][r]
            sample, period, _eff, _param = mod_cell_decode(cell)
            if period != 0 and sample != 0:
                out.append((r, sample, period))
        return sorted(out)

    def data(self):
        out = bytearray()
        for row in range(self.ROWS):
            for ch in range(self.CHANNELS):
                out += self.cells[ch].get(row, b"\x00\x00\x00\x00")
        return bytes(out)


class ModSample(object):
    def __init__(self, name, data8, volume=64, loop=False):
        self.name = name[:22]
        self.data = data8
        if len(data8) % 2:
            self.data += b"\x00"
        self.volume = clamp(int(volume), 0, 64)
        self.loop = loop
        self.loop_start = 0
        self.loop_len = (len(self.data) // 2) if loop else 0

    def header(self):
        n = self.name.encode("ascii", "replace").ljust(22, b"\x00")[:22]
        wl = (len(self.data) // 2) & 0xFFFF
        ls = (self.loop_start // 2) & 0xFFFF
        ll = (self.loop_len // 2) & 0xFFFF
        return n + struct.pack(">HBBHH", wl, 0, self.volume, ls, ll)


def build_mod(title, samples, patterns, orders, restart=127):
    """Serialize a complete ProTracker M.K. module."""
    if len(samples) > 31:
        raise ValueError("MOD allows at most 31 samples")
    out = bytearray()
    out += title.encode("ascii", "replace")[:20].ljust(20, b"\x00")
    for i in range(31):
        out += samples[i].header() if i < len(samples) else b"\x00" * 30
    out += bytes([len(orders) & 0x7F, restart & 0xFF])
    order_tbl = bytearray(128)
    for i, o in enumerate(orders):
        order_tbl[i] = o
    out += bytes(order_tbl)
    out += b"M.K."
    for p in patterns:
        out += p.data()
    for s in samples:
        out += s.data
    return bytes(out)


def write_mod(path, data):
    with open(path, "wb") as f:
        f.write(data)


# --------------------------------------------------------------------------
# MOD instrument waveforms
# --------------------------------------------------------------------------

def additive_cycle(n, partials):
    """One cycle of length `n` built from (harmonic, amplitude) partials."""
    buf = [0.0] * n
    for k, amp in partials:
        for i in range(n):
            buf[i] += amp * math.sin(TAU * k * i / n)
    m = max(abs(v) for v in buf)
    return [v / m for v in buf]


def to_8bit(samples, peak):
    """Float buffer -> signed 8-bit bytes (peak = fraction of 127)."""
    buf = norm_peak(samples, peak)
    out = bytearray()
    for v in buf:
        s = int(round(v * 127.0))
        out.append(clamp(s, -127, 127) & 0xFF)
    return bytes(out)


# One lead cycle = 128 samples, so playback rate = 8363*1712/P and the
# fundamental is 8363*1712/(128*P) Hz -- exactly the tempered pitch of the
# period table entry P (P=428 -> 261.3 Hz = C4, P=214 -> 522.7 Hz = C5).
LEAD_CYCLE = 128
BASS_CYCLES = 2

LEAD_PARTIALS = [(1, 1.00), (2, 0.10), (3, 0.34), (4, 0.06),
                 (5, 0.18), (6, 0.04), (7, 0.10), (8, 0.03)]
BASS_PARTIALS = [(1, 1.00), (2, 0.32), (3, 0.15), (4, 0.05), (5, 0.02)]
PAD_PARTIALS = [(1, 1.00), (2, 0.24), (3, 0.22), (4, 0.05), (5, 0.07)]


def make_lead(rng=None):
    return additive_cycle(LEAD_CYCLE, LEAD_PARTIALS)


def make_bass(rng=None):
    cyc = additive_cycle(LEAD_CYCLE, BASS_PARTIALS)
    return cyc * BASS_CYCLES          # 256 samples = 2 cycles, still click-free


def make_pad(rng=None):
    return additive_cycle(LEAD_CYCLE, PAD_PARTIALS)


def make_kick(rng):
    """Short low sine sweep, designed to be played at period 1712 (8363 Hz)."""
    sr = MOD_BASE_RATE
    n = int(0.14 * sr)
    buf = add_synth(n, sr,
                    lambda i: 47.0 + 105.0 * math.exp(-i / (0.022 * sr)),
                    [(1, 1.0), (2, 0.13)], attack=0.0015)
    buf = mul_env(buf, env_ar(n, sr, attack=0.0015, tau=0.042))
    click = mul_env(noise(n, rng), env_ar(n, sr, attack=0.0002, tau=0.0025))
    return [b + 0.30 * c for b, c in zip(buf, click)]


def make_snare(rng):
    """Snare, designed to be played at period 856 (16726 Hz)."""
    sr = MOD_BASE_RATE * 2
    n = int(0.09 * sr)
    body = mul_env(noise(n, rng), env_ar(n, sr, attack=0.0005, tau=0.032))
    body = lowpass(body, 6500.0, sr)
    snap = differentiate(highpass(noise(n, rng), 0.9), 1)
    snap = mul_env(snap, env_ar(n, sr, attack=0.0003, tau=0.014))
    tone = add_synth(n, sr, lambda i: 196.0, [(1, 1.0), (2, 0.2)], attack=0.001)
    tone = mul_env(tone, env_ar(n, sr, attack=0.001, tau=0.020))
    return [0.85 * b + 0.55 * s + 0.45 * t for b, s, t in zip(body, snap, tone)]


def make_hat(rng):
    """Closed hi-hat, designed to be played at period 856 (16726 Hz)."""
    sr = MOD_BASE_RATE * 2
    n = int(0.035 * sr)
    h = differentiate(noise(n, rng), 2)
    h = mul_env(h, env_ar(n, sr, attack=0.0003, tau=0.009))
    return h


# --------------------------------------------------------------------------
# song construction helpers
# --------------------------------------------------------------------------

BAR = 16          # rows per 4/4 bar (row = sixteenth note)


def perc_rows(pat, ch, sample, period, rows, gain_row=None):
    for r in rows:
        pat.put(ch, r, sample, period=period)


def kick_snare(pat, bar_rows=range(0, 64, BAR)):
    """Kick on beats 1/3, snare on 2/4 of every bar, on channel 2."""
    kicks, snares = [], []
    for b in bar_rows:
        kicks += [b, b + 8]
        snares += [b + 4, b + 12]
    perc_rows(pat, 2, KICK, 1712, kicks)
    perc_rows(pat, 2, SNARE, 856, snares)


def hats(pat, step=2):
    perc_rows(pat, 3, HAT, 856, range(0, 64, step))


def bass_bar(pat, ch, root_row, note_a, note_b, half=8):
    pat.put(ch, root_row, BASS, note=note_a)
    pat.put(ch, root_row + half, BASS, note=note_b)


def arp_run(pat, ch, sample, start, notes, step=2):
    for k, n in enumerate(notes):
        pat.put(ch, start + k * step, sample, note=n)


# --------------------------------------------------------------------------
# the six songs
# --------------------------------------------------------------------------

def song_title():
    """Warm, gentle, welcoming. C major, speed 6 (125 BPM), I-vi-IV-V."""
    pats = [Pattern() for _ in range(3)]

    # --- pattern 0: half-note melody, one bar per chord -------------------
    pats[0].notes(0, LEAD, [(0, "E4"), (8, "G4"), (16, "A4"), (24, "C5"),
                            (32, "F4"), (40, "A4"), (48, "D5"), (56, "B4")])
    # --- pattern 1: same harmony, melody lifted into a higher register ----
    pats[1].notes(0, LEAD, [(0, "G4"), (8, "C5"), (16, "E5"), (24, "C5"),
                            (32, "A4"), (40, "C5"), (48, "B4"), (56, "D5")])
    # --- pattern 2: gentle F-G-C-Am answering phrase ---------------------
    pats[2].notes(0, LEAD, [(0, "C5"), (8, "A4"), (16, "B4"), (24, "D5"),
                            (32, "E5"), (40, "C5"), (48, "C5"), (56, "A4")])

    bass_lines = [
        [("C3", "G3"), ("A3", "E3"), ("F3", "C4"), ("G3", "D4")],
    ]
    for pat, chords in zip(pats, bass_lines * 3):
        for bar, (a, b) in enumerate(chords):
            bass_bar(pat, 1, bar * BAR, a, b)

    for pat in pats:
        perc_rows(pat, 2, KICK, 1712, [0, 32])              # two soft kicks
        perc_rows(pat, 3, HAT, 856, [8, 24, 40, 56])        # brushed quarters

    return dict(name="title", speed=6, samples=[
        (LEAD, "capy lead", make_lead(), 52, True),
        (BASS, "capy bass", make_bass(), 60, True),
        (KICK, "kick", make_kick, 52, False),
        (SNARE, "snare", make_snare, 34, False),
        (HAT, "hat", make_hat, 20, False),
    ], patterns=pats, orders=[0, 1, 2, 1])


def song_field():
    """Cheerful walking / adventure theme. G major, speed 5 (150 BPM)."""
    pats = [Pattern() for _ in range(3)]

    # bouncy eighth-note arpeggios, G | Em | C | D
    pats[0].notes(0, LEAD, [
        (0, "B4"), (2, "D5"), (4, "G5"), (6, "D5"), (8, "B4"), (10, "D5"), (12, "G5"),
        (16, "A4"), (18, "B4"), (20, "E5"), (22, "B4"), (24, "A4"), (26, "B4"), (28, "E5"),
        (32, "C5"), (34, "E5"), (36, "G5"), (38, "E5"), (40, "C5"), (42, "E5"), (44, "G5"),
        (48, "D5"), (50, "F#5"), (52, "A4"), (54, "D5"), (56, "F#5"), (58, "A4"), (60, "D5"),
    ])
    # walking quarter notes (the "stroll")
    pats[1].notes(0, LEAD, [
        (0, "G4"), (4, "B4"), (8, "D5"), (12, "G5"),
        (16, "E5"), (20, "D5"), (24, "B4"), (28, "G4"),
        (32, "C5"), (36, "E5"), (40, "G5"), (44, "E5"),
        (48, "D5"), (52, "F#5"), (56, "A4"), (60, "D5"),
    ])
    # variation on pattern 0, melody starts on the fifth
    pats[2].notes(0, LEAD, [
        (0, "D5"), (2, "B4"), (4, "G4"), (6, "B4"), (8, "D5"), (10, "B4"), (12, "G4"),
        (16, "G4"), (18, "B4"), (20, "E5"), (22, "B4"), (24, "G4"), (26, "B4"), (28, "E5"),
        (32, "E5"), (34, "G5"), (36, "E5"), (38, "C5"), (40, "E5"), (42, "G5"), (44, "G5"),
        (48, "A4"), (50, "D5"), (52, "F#5"), (54, "A4"), (56, "D5"), (58, "F#5"), (60, "A4"),
    ])

    bass = [("G2", "D3"), ("E2", "B2"), ("C3", "G2"), ("D3", "A2")]
    for pat in pats:
        for bar, (r, f) in enumerate(bass):
            base = bar * BAR
            pat.put(1, base + 0, BASS, note=r)
            pat.put(1, base + 4, BASS, note=f)
            pat.put(1, base + 8, BASS, note=r)
            pat.put(1, base + 12, BASS, note=f)
        kick_snare(pat)
        hats(pat, 2)

    return dict(name="field", speed=5, samples=[
        (LEAD, "capy lead", make_lead(), 56, True),
        (BASS, "capy bass", make_bass(), 62, True),
        (KICK, "kick", make_kick, 56, False),
        (SNARE, "snare", make_snare, 40, False),
        (HAT, "hat", make_hat, 24, False),
    ], patterns=pats, orders=[0, 1, 2, 1])


def song_battle():
    """Faster, urgent. A minor, speed 4 (187 BPM), driving eighth-note bass."""
    pats = [Pattern() for _ in range(3)]

    # Am | F | G | Am
    pats[0].notes(0, LEAD, [
        (0, "A4"), (2, "C5"), (4, "E5"), (6, "A5"), (8, "E5"), (10, "C5"), (12, "A4"), (14, "C5"),
        (16, "F5"), (18, "C5"), (20, "A4"), (22, "F5"), (24, "A4"), (26, "C5"), (28, "F5"), (30, "C5"),
        (32, "G5"), (34, "D5"), (36, "B4"), (38, "G5"), (40, "D5"), (42, "B4"), (44, "G4"), (46, "B4"),
        (48, "A4"), (50, "E5"), (52, "A5"), (54, "E5"), (56, "A4"), (58, "C5"), (60, "E5"), (62, "A5"),
    ])
    # Am | F | E7 | E7 -- the harmonic-minor tension lift
    pats[1].notes(0, LEAD, [
        (0, "A4"), (2, "C5"), (4, "E5"), (6, "A5"), (8, "A5"), (10, "A5"), (12, "G5"), (14, "E5"),
        (16, "F5"), (18, "F5"), (20, "E5"), (22, "C5"), (24, "A4"), (26, "C5"), (28, "F5"), (30, "F5"),
        (32, "E5"), (34, "G#5"), (36, "B4"), (38, "E5"), (40, "G#5"), (42, "B4"), (44, "E5"), (46, "D5"),
        (48, "E5"), (50, "E5"), (52, "D5"), (54, "C5"), (56, "B4"), (58, "G#4"), (60, "A4"), (62, "B4"),
    ])
    # Am | F | G | Am, insistent repeated-note figure
    pats[2].notes(0, LEAD, [
        (0, "A4"), (2, "A4"), (4, "C5"), (6, "C5"), (8, "E5"), (10, "E5"), (12, "A5"), (14, "A5"),
        (16, "F5"), (18, "F5"), (20, "E5"), (22, "E5"), (24, "D5"), (26, "D5"), (28, "C5"), (30, "C5"),
        (32, "B4"), (34, "B4"), (36, "D5"), (38, "D5"), (40, "G5"), (42, "G5"), (44, "D5"), (46, "B4"),
        (48, "A4"), (50, "E5"), (52, "A5"), (54, "E5"), (56, "C5"), (58, "A4"), (60, "E5"), (62, "A5"),
    ])

    roots = [["A2"], ["F2"], ["G2"], ["A2"]]
    for pat in pats:
        for bar in range(4):
            base = bar * BAR
            root = roots[bar][0]
            # galloping eighths: root-root-octave-root
            for k, off in enumerate((0, 2, 4, 6, 8, 10, 12, 14)):
                pat.put(1, base + off, BASS, note=root if k % 4 != 2 else root[:-1] + "3")
        kick_snare(pat)
        hats(pat, 2)

    return dict(name="battle", speed=4, samples=[
        (LEAD, "capy lead", make_lead(), 58, True),
        (BASS, "capy bass", make_bass(), 64, True),
        (KICK, "kick", make_kick, 60, False),
        (SNARE, "snare", make_snare, 44, False),
        (HAT, "hat", make_hat, 26, False),
    ], patterns=pats, orders=[0, 1, 0, 2])


def song_boss():
    """Dramatic D minor, heavy low bass, tense melody, speed 4."""
    pats = [Pattern() for _ in range(3)]

    # Dm | Bb | Gm | A
    pats[0].notes(0, LEAD, [
        (0, "A4"), (2, "A4"), (4, "D5"), (6, "F5"), (8, "E5"), (10, "D5"), (12, "A4"), (14, "F5"),
        (16, "A#4"), (18, "A#4"), (20, "D5"), (22, "F5"), (24, "D5"), (26, "A#4"), (28, "F5"), (30, "D5"),
        (32, "G4"), (34, "G4"), (36, "A#4"), (38, "D5"), (40, "A#4"), (42, "G4"), (44, "D5"), (46, "A#4"),
        (48, "A4"), (50, "A4"), (52, "C#5"), (54, "E5"), (56, "C#5"), (58, "A4"), (60, "E5"), (62, "A4"),
    ])
    # Dm | Bb | A | A -- agitated high register
    pats[1].notes(0, LEAD, [
        (0, "D5"), (2, "D5"), (4, "F5"), (6, "A5"), (8, "F5"), (10, "D5"), (12, "A4"), (14, "D5"),
        (16, "F5"), (18, "F5"), (20, "D5"), (22, "A#4"), (24, "D5"), (26, "F5"), (28, "D5"), (30, "F5"),
        (32, "E5"), (34, "E5"), (36, "C#5"), (38, "A4"), (40, "C#5"), (42, "E5"), (44, "A5"), (46, "E5"),
        (48, "E5"), (50, "C#5"), (52, "A4"), (54, "E5"), (56, "C#5"), (58, "A4"), (60, "E5"), (62, "A5"),
    ])
    # Dm | Gm | Bb | A -- heavy descending answer
    pats[2].notes(0, LEAD, [
        (0, "D5"), (2, "C5"), (4, "A4"), (6, "F4"), (8, "A4"), (10, "C5"), (12, "D5"), (14, "A4"),
        (16, "G4"), (18, "A#4"), (20, "D5"), (22, "A#4"), (24, "G4"), (26, "A#4"), (28, "D5"), (30, "G5"),
        (32, "F5"), (34, "D5"), (36, "A#4"), (38, "F5"), (40, "D5"), (42, "A#4"), (44, "D5"), (46, "F5"),
        (48, "E5"), (50, "E5"), (52, "D5"), (54, "C#5"), (56, "A4"), (58, "C#5"), (60, "E5"), (62, "A5"),
    ])

    # heavy "power" bass: root root root octave, one chord per bar
    bass_roots = [("D2", "D3"), ("A#2", "A#3"), ("G2", "G3"), ("A2", "A3")]
    # colour tone for the pad channel (third of each chord)
    pad_tones = ["F3", "D4", "A#3", "C#4"]
    for pat in pats:
        for bar in range(4):
            base = bar * BAR
            low, high = bass_roots[bar]
            for off, note in ((0, low), (4, low), (8, low), (12, high)):
                pat.put(1, base + off, BASS, note=note)
            pat.put(3, base + 0, PAD, note=pad_tones[bar])
            pat.put(3, base + 8, PAD, note=pad_tones[bar])
        kick_snare(pat)           # no hats: keep it heavy

    return dict(name="boss", speed=4, samples=[
        (LEAD, "capy lead", make_lead(), 60, True),
        (BASS, "capy bass", make_bass(), 64, True),
        (KICK, "kick", make_kick, 62, False),
        (SNARE, "snare", make_snare, 46, False),
        (PAD, "pad", make_pad, 26, True),
    ], patterns=pats, orders=[0, 1, 0, 2])


def song_victory():
    """Short triumphant fanfare, C major, speed 6. Plays once (no loop)."""
    pat = Pattern()
    # ta-da: rising triad then a held high C
    pat.notes(0, LEAD, [
        (0, "G4"), (2, "C5"), (4, "E5"), (6, "G5"),
        (8, "C6"), (12, "G5"), (16, "C6"),
        (24, "E5"), (26, "G5"), (28, "C6"), (32, "C6"),
    ])
    # strong root motion: C  C  G  C  (then hold the tonic)
    for row, note in ((0, "C3"), (8, "C3"), (16, "G2"), (24, "C3"), (32, "C3")):
        pat.put(1, row, BASS, note=note)
    # sustained pad: third -> fifth -> fifth, supporting the final chord
    for row, note in ((0, "E4"), (16, "G4"), (32, "G4")):
        pat.put(3, row, PAD, note=note)
    # drums: four on the floor plus a snare roll into the last chord
    perc_rows(pat, 2, KICK, 1712, [0, 8, 16, 24, 32])
    perc_rows(pat, 2, SNARE, 856, [4, 12, 20, 26, 28, 30])

    return dict(name="victory", speed=6, samples=[
        (LEAD, "capy lead", make_lead(), 58, True),
        (BASS, "capy bass", make_bass(), 62, True),
        (KICK, "kick", make_kick, 58, False),
        (SNARE, "snare", make_snare, 46, False),
        (PAD, "pad", make_pad, 28, True),
    ], patterns=[pat], orders=[0])


def song_birthday():
    """Happy Birthday to You (public domain). C major, speed 6, row = sixteenth.

    Melody on channel 0 with the classic dotted-eighth + sixteenth "hap-py"
    rhythm, chord-tone bass on channel 1 (C / G7 / F / C roots), and only a
    whisper of percussion.  Loops back to the top.
    """
    pats = [Pattern(), Pattern()]

    # (row_offset, note) for each 24-row phrase; 3+1 rows = dotted eighth.
    phrases = [
        # "Hap-py birth-day to you"
        [(0, "G4"), (3, "G4"), (4, "A4"), (8, "G4"), (12, "C5"), (16, "B4")],
        # "Hap-py birth-day to you"
        [(0, "G4"), (3, "G4"), (4, "A4"), (8, "G4"), (12, "D5"), (16, "C5")],
        # "Hap-py birth-day dear [name]"
        [(0, "G4"), (3, "G4"), (4, "G5"), (8, "E5"), (12, "C5"), (16, "B4"), (20, "A4")],
        # "Hap-py birth-day to you"
        [(0, "F5"), (3, "F5"), (4, "E5"), (8, "C5"), (12, "D5"), (16, "C5")],
    ]

    # bass: one root per half-measure (two chords per 24-row phrase)
    bass_roots = [("C3", "G3"), ("G3", "C3"), ("C3", "F3"), ("F3", "C3")]

    for p_idx in (0, 1):
        pat = pats[p_idx]
        for ph in range(2):                      # two phrases per pattern
            gi = p_idx * 2 + ph
            if gi >= len(phrases):
                continue
            base = ph * 24
            pat.notes(0, LEAD, [(base + r, n) for r, n in phrases[gi]])
            low, high = bass_roots[gi]
            for m in range(2):                   # each phrase = two measures
                mbase = base + m * 12
                pat.put(1, mbase, BASS, note=low if m == 0 else high)
                pat.put(1, mbase + 6, BASS, note=low if m == 0 else high)
        # a breath of percussion: soft kick on each measure downbeat
        perc_rows(pat, 2, KICK, 1712, [0, 12, 24, 36, 48, 60])

    # pattern 1 row 48.. : closing tonic in the bass, then silence, then loop
    pats[1].put(1, 48, BASS, note="C3")
    pats[1].put(0, 48, LEAD, note="C5")

    return dict(name="birthday", speed=6, samples=[
        (LEAD, "capy lead", make_lead(), 56, True),
        (BASS, "capy bass", make_bass(), 58, True),
        (KICK, "kick", make_kick, 26, False),      # deliberately very soft
    ], patterns=pats, orders=[0, 1])


# --------------------------------------------------------------------------
# one battle theme per region
#
# Five regions sharing one battle track was the one gap in an otherwise fully
# themed region package (atlas, weather, roster, boss all differ). These are
# not transpositions of one tune: each region gets its own key, its own
# tempo and its own melodic figures, written over the same chassis as
# song_battle (three 4-bar patterns, galloping bass, kick/snare/hats) so the
# bank stays small and the family resemblance is deliberate -- five dialects
# of the same language, not five languages.
# --------------------------------------------------------------------------

def region_battle(name, speed, roots, mel_a, mel_b, mel_c):
    """Build a battle theme from 4 bass roots and three melody lines.

    `roots` are the per-bar bass roots (octave 2); the gallop octave-jumps
    them to 3, exactly as song_battle does. Each melody list is 32
    (row, note) pairs over the 64-row pattern -- eighth notes, chord tones
    on the strong rows.
    """
    pats = [Pattern() for _ in range(3)]
    for pat, mel in ((pats[0], mel_a), (pats[1], mel_b), (pats[2], mel_c)):
        pat.notes(0, LEAD, mel)
        for bar in range(4):
            base = bar * BAR
            root = roots[bar]
            for k, off in enumerate((0, 2, 4, 6, 8, 10, 12, 14)):
                pat.put(1, base + off, BASS,
                        note=root if k % 4 != 2 else root[:-1] + "3")
        kick_snare(pat)
        hats(pat, 2)

    return dict(name=name, speed=speed, samples=[
        (LEAD, "capy lead", make_lead(), 58, True),
        (BASS, "capy bass", make_bass(), 64, True),
        (KICK, "kick", make_kick, 60, False),
        (SNARE, "snare", make_snare, 44, False),
        (HAT, "hat", make_hat, 26, False),
    ], patterns=pats, orders=[0, 1, 0, 2])


def song_battle_rain():
    """雨林. D dorian, speed 4. Rolling and green: wide arpeggios that keep
    circling back to D, with the dorian 6th (B natural) colouring bar 4."""
    return region_battle(
        "battle_rain", 4,
        ["D2", "F2", "C2", "G2"],
        # A: rolling chord arpeggios, one chord per bar
        [(0, "D5"), (2, "F5"), (4, "A4"), (6, "D5"), (8, "F5"), (10, "A5"),
         (12, "F5"), (14, "D5"),
         (16, "A4"), (18, "C5"), (20, "F5"), (22, "A5"), (24, "F5"),
         (26, "C5"), (28, "A4"), (30, "F4"),
         (32, "E5"), (34, "G5"), (36, "C5"), (38, "E5"), (40, "G4"),
         (42, "C5"), (44, "E5"), (46, "G5"),
         (48, "B4"), (50, "D5"), (52, "G5"), (54, "D5"), (56, "B4"),
         (58, "G4"), (60, "A4"), (62, "B4")],
        # B: insistent repeated-note lift
        [(0, "D5"), (2, "D5"), (4, "A4"), (6, "A4"), (8, "D5"), (10, "D5"),
         (12, "F5"), (14, "F5"),
         (16, "C5"), (18, "C5"), (20, "A4"), (22, "A4"), (24, "F5"),
         (26, "F5"), (28, "C5"), (30, "C5"),
         (32, "E5"), (34, "E5"), (36, "G4"), (38, "G4"), (40, "E5"),
         (42, "E5"), (44, "C5"), (46, "C5"),
         (48, "D5"), (50, "D5"), (52, "B4"), (54, "G4"), (56, "A4"),
         (58, "B4"), (60, "C5"), (62, "D5")],
        # C: descending answering phrase
        [(0, "F5"), (2, "E5"), (4, "D5"), (6, "C5"), (8, "D5"), (10, "A4"),
         (12, "B4"), (14, "C5"),
         (16, "A4"), (18, "C5"), (20, "F5"), (22, "C5"), (24, "A4"),
         (26, "F4"), (28, "G4"), (30, "A4"),
         (32, "G4"), (34, "C5"), (36, "E5"), (38, "G5"), (40, "E5"),
         (42, "C5"), (44, "G4"), (46, "C5"),
         (48, "D5"), (50, "B4"), (52, "G4"), (54, "B4"), (56, "D5"),
         (58, "G5"), (60, "F5"), (62, "D5")],
    )


def song_battle_farm():
    """农场. G major, speed 5. A harvest reel: bouncing major-key gallop,
    melodies that hop like sparrows after thresher grain."""
    return region_battle(
        "battle_farm", 5,
        ["G2", "E2", "C2", "D2"],
        # A: G | Em | C | D, hopping chord tones
        [(0, "G4"), (2, "B4"), (4, "D5"), (6, "G5"), (8, "D5"), (10, "B4"),
         (12, "D5"), (14, "G4"),
         (16, "E5"), (18, "B4"), (20, "G4"), (22, "E4"), (24, "G4"),
         (26, "B4"), (28, "E5"), (30, "B4"),
         (32, "C5"), (34, "E5"), (36, "G5"), (38, "E5"), (40, "C5"),
         (42, "G4"), (44, "E5"), (46, "C5"),
         (48, "D5"), (50, "F#5"), (52, "A5"), (54, "F#5"), (56, "D5"),
         (58, "A4"), (60, "F#4"), (62, "A4")],
        # B: the reel turns over -- repeated pairs, rising answer
        [(0, "G5"), (2, "G5"), (4, "D5"), (6, "D5"), (8, "B4"), (10, "B4"),
         (12, "G5"), (14, "G5"),
         (16, "E5"), (18, "E5"), (20, "B4"), (22, "B4"), (24, "G4"),
         (26, "G4"), (28, "E5"), (30, "E5"),
         (32, "C5"), (34, "C5"), (36, "G4"), (38, "G4"), (40, "E5"),
         (42, "E5"), (44, "C5"), (46, "C5"),
         (48, "D5"), (50, "D5"), (52, "F#4"), (54, "A4"), (56, "D5"),
         (58, "F#5"), (60, "A5"), (62, "F#5")],
        # C: the reel's big descending swing
        [(0, "D5"), (2, "C5"), (4, "B4"), (6, "A4"), (8, "G4"), (10, "B4"),
         (12, "D5"), (14, "G5"),
         (16, "E5"), (18, "D5"), (20, "B4"), (22, "G4"), (24, "E4"),
         (26, "G4"), (28, "B4"), (30, "E5"),
         (32, "G5"), (34, "E5"), (36, "C5"), (38, "G4"), (40, "C5"),
         (42, "E5"), (44, "G5"), (46, "C6"),
         (48, "D5"), (50, "F#5"), (52, "A5"), (54, "D6"), (56, "A5"),
         (58, "F#5"), (60, "D5"), (62, "D5")],
    )


def song_battle_past():
    """牧场. C major, speed 5. Open plains: wide leaps, a tune that lopes
    like something running because it can, not because it must."""
    return region_battle(
        "battle_past", 5,
        ["C2", "A2", "F2", "G2"],
        # A: C | Am | F | G, leaping thirds and sixths
        [(0, "C5"), (2, "E5"), (4, "G4"), (6, "C5"), (8, "E5"), (10, "G5"),
         (12, "E5"), (14, "C5"),
         (16, "A4"), (18, "C5"), (20, "E5"), (22, "A5"), (24, "E5"),
         (26, "C5"), (28, "A4"), (30, "E4"),
         (32, "F4"), (34, "A4"), (36, "C5"), (38, "F5"), (40, "C5"),
         (42, "A4"), (44, "F4"), (46, "C5"),
         (48, "D5"), (50, "G4"), (52, "B4"), (54, "D5"), (56, "G5"),
         (58, "D5"), (60, "B4"), (62, "G4")],
        # B: galloping repeated figures across the plain
        [(0, "E5"), (2, "E5"), (4, "C5"), (6, "C5"), (8, "G4"), (10, "G4"),
         (12, "E5"), (14, "E5"),
         (16, "C5"), (18, "C5"), (20, "A4"), (22, "A4"), (24, "E5"),
         (26, "E5"), (28, "C5"), (30, "C5"),
         (32, "A4"), (34, "A4"), (36, "F4"), (38, "F4"), (40, "C5"),
         (42, "C5"), (44, "A4"), (46, "A4"),
         (48, "B4"), (50, "B4"), (52, "G4"), (54, "G4"), (56, "D5"),
         (58, "D5"), (60, "B4"), (62, "D5")],
        # C: the wide answering phrase, top register
        [(0, "G5"), (2, "E5"), (4, "C5"), (6, "G4"), (8, "C5"), (10, "E5"),
         (12, "G5"), (14, "E5"),
         (16, "A5"), (18, "E5"), (20, "C5"), (22, "A4"), (24, "C5"),
         (26, "E5"), (28, "A5"), (30, "E5"),
         (32, "F5"), (34, "C5"), (36, "A4"), (38, "F4"), (40, "A4"),
         (42, "C5"), (44, "F5"), (46, "C5"),
         (48, "G5"), (50, "D5"), (52, "B4"), (54, "G4"), (56, "B4"),
         (58, "D5"), (60, "G5"), (62, "B5")],
    )


def song_battle_marsh():
    """沼泽. E minor, speed 4. Lurking: a creeping half-step figure over a
    slow-for-its-tempo harmonic rhythm, with the harmonic-minor lift into B."""
    return region_battle(
        "battle_marsh", 4,
        ["E2", "C2", "A2", "B2"],
        # A: Em | C | Am | B -- the creep, then the dominant
        [(0, "E5"), (2, "D#5"), (4, "E5"), (6, "G4"), (8, "B4"), (10, "E5"),
         (12, "D5"), (14, "B4"),
         (16, "C5"), (18, "B4"), (20, "C5"), (22, "E4"), (24, "G4"),
         (26, "C5"), (28, "B4"), (30, "G4"),
         (32, "A4"), (34, "G#4"), (36, "A4"), (38, "C4"), (40, "E4"),
         (42, "A4"), (44, "G4"), (46, "E4"),
         (48, "F#4"), (50, "B4"), (52, "D#5"), (54, "F#5"), (56, "B4"),
         (58, "D#5"), (60, "F#5"), (62, "A5")],
        # B: held tension, repeated notes over the turn
        [(0, "B4"), (2, "B4"), (4, "E5"), (6, "D#5"), (8, "E5"), (10, "G5"),
         (12, "E5"), (14, "B4"),
         (16, "G4"), (18, "G4"), (20, "C5"), (22, "B4"), (24, "C5"),
         (26, "E5"), (28, "C5"), (30, "G4"),
         (32, "E4"), (34, "E4"), (36, "A4"), (38, "G#4"), (40, "A4"),
         (42, "C5"), (44, "A4"), (46, "E4"),
         (48, "F#4"), (50, "F#4"), (52, "B4"), (54, "A#4"), (56, "B4"),
         (58, "D#5"), (60, "F#5"), (62, "B5")],
        # C: the answer crawls back down
        [(0, "G5"), (2, "F#5"), (4, "E5"), (6, "D#5"), (8, "E5"), (10, "B4"),
         (12, "G4"), (14, "B4"),
         (16, "E5"), (18, "D5"), (20, "C5"), (22, "B4"), (24, "C5"),
         (26, "G4"), (28, "E4"), (30, "G4"),
         (32, "C5"), (34, "B4"), (36, "A4"), (38, "G#4"), (40, "A4"),
         (42, "E4"), (44, "C4"), (46, "E4"),
         (48, "D#5"), (50, "F#5"), (52, "B4"), (54, "F#4"), (56, "B4"),
         (58, "F#5"), (60, "D#5"), (62, "B4")],
    )


def song_battle_volcano():
    """可可火山. F minor, speed 3. The fastest thing in the bank: Fm | Db |
    Eb | Fm with the harmonic-minor E natural over the dominant, relentless
    driving eighths -- the final region's soundtrack should sound like the
    ground under it."""
    return region_battle(
        "battle_volcano", 3,
        ["F2", "C#2", "D#2", "F2"],
        # A: driving arpeggios
        [(0, "F5"), (2, "C5"), (4, "G#4"), (6, "F4"), (8, "G#4"),
         (10, "C5"), (12, "F5"), (14, "C5"),
         (16, "G#5"), (18, "F5"), (20, "C#5"), (22, "G#4"), (24, "C#5"),
         (26, "F5"), (28, "G#5"), (30, "F5"),
         (32, "D#5"), (34, "G4"), (36, "D#5"), (38, "G5"), (40, "D#5"),
         (42, "G4"), (44, "A#4"), (46, "D#5"),
         (48, "F5"), (50, "C5"), (52, "G#4"), (54, "F4"), (56, "F5"),
         (58, "C6"), (60, "G#5"), (62, "F5")],
        # B: the eruption -- repeated peaks, harmonic-minor E natural
        [(0, "F5"), (2, "F5"), (4, "C6"), (6, "C6"), (8, "G#5"), (10, "G#5"),
         (12, "F5"), (14, "F5"),
         (16, "C#5"), (18, "C#5"), (20, "G#5"), (22, "G#5"), (24, "F5"),
         (26, "F5"), (28, "C#5"), (30, "C#5"),
         (32, "D#5"), (34, "D#5"), (36, "G5"), (38, "G5"), (40, "D#5"),
         (42, "D#5"), (44, "A#4"), (46, "A#4"),
         (48, "E5"), (50, "E5"), (52, "G5"), (54, "C5"), (56, "B4"),
         (58, "C5"), (60, "F5"), (62, "C5")],
        # C: falling ash -- descending pairs back to the root
        [(0, "G#5"), (2, "G#5"), (4, "F5"), (6, "F5"), (8, "C5"),
         (10, "C5"), (12, "G#4"), (14, "G#4"),
         (16, "F5"), (18, "F5"), (20, "C#5"), (22, "C#5"), (24, "G#4"),
         (26, "G#4"), (28, "C#5"), (30, "F5"),
         (32, "G5"), (34, "G5"), (36, "D#5"), (38, "D#5"), (40, "A#4"),
         (42, "A#4"), (44, "D#5"), (46, "G5"),
         (48, "F5"), (50, "C5"), (52, "G#4"), (54, "F4"), (56, "G#4"),
         (58, "C5"), (60, "F5"), (62, "C6")],
    )


SONG_BUILDERS = [song_title, song_field, song_battle,
                 song_boss, song_victory, song_birthday,
                 song_battle_rain, song_battle_farm, song_battle_past,
                 song_battle_marsh, song_battle_volcano]

# songs that must loop forever: the last row of the last pattern jumps home.
# (victory is a one-shot jingle, so it is excluded.)
LOOPING_SONGS = {"title", "field", "battle", "boss", "birthday",
                 "battle_rain", "battle_farm", "battle_past",
                 "battle_marsh", "battle_volcano"}


def realize_song(spec, rng):
    """Turn a song spec into (mod_bytes, samples, patterns, orders, speed)."""
    by_slot = {}
    for slot, sname, maker, vol, loop in spec["samples"]:
        raw = maker(rng) if callable(maker) else maker
        peak = 0.86 if loop else 0.92
        by_slot[slot] = ModSample(sname, to_8bit(raw, peak), vol, loop=loop)

    pats = spec["patterns"]
    orders = list(spec["orders"])

    # Effect Fxx at row 0 of the first pattern sets the tempo explicitly.
    # Pattern.put() replaces the whole cell, so this must go on a channel with
    # no note on the downbeat -- writing it over the melody's first note would
    # silently delete it (which is exactly what happened to the opening note of
    # the birthday tune). If every channel has a note there, merge the effect
    # into an existing cell instead.
    if "speed" in spec:
        first = pats[orders[0]]
        placed = False
        for ch in range(Pattern.CHANNELS - 1, -1, -1):
            if 0 not in first.cells[ch]:
                first.put(ch, 0, 0, period=0, effect=0x0F,
                          param=spec["speed"])
                placed = True
                break
        if not placed:
            sample, period, _e, _pa = mod_cell_decode(first.cells[0][0])
            first.cells[0][0] = mod_cell(sample, period, 0x0F, spec["speed"])

    if spec["name"] in LOOPING_SONGS:
        # Bxx on the last row of the final pattern jumps back to position 0,
        # so the tune loops even in players that ignore maxmod's loop mode.
        last = pats[orders[-1]]
        last.put(0, 63, 0, period=0, effect=0x0B, param=0)

    # ProTracker sample numbers are 1-based and must be consecutive, so a spec
    # that skips a slot (a song with no hi-hat, say) gets its slots compacted
    # here and every pattern cell is rewritten to match.
    old_slots = sorted(by_slot)
    slot_map = {old: new for new, old in enumerate(old_slots, start=1)}
    if old_slots != list(range(1, len(old_slots) + 1)):
        for pat in pats:
            for ch in range(Pattern.CHANNELS):
                for row, cell in list(pat.cells[ch].items()):
                    sample, period, effect, param = mod_cell_decode(cell)
                    if sample:
                        sample = slot_map[sample]
                    pat.cells[ch][row] = mod_cell(sample, period, effect, param)

    samples = [by_slot[k] for k in old_slots]
    data = build_mod(spec["name"], samples, pats, orders)
    return data, samples, pats, orders, spec.get("speed", 6)


# --------------------------------------------------------------------------
# module reader + preview renderer (verification only)
# --------------------------------------------------------------------------

def read_mod(path):
    """Parse a MOD file back out of its bytes (validates the writer)."""
    d = open(path, "rb").read()
    n_samples = 31
    assert d[1080:1084] == b"M.K.", "bad magic"
    orders = list(d[952:952 + 128])
    song_len, _restart = d[950], d[951]
    orders = orders[:song_len]
    pat_bytes = 64 * 4 * 4
    n_pat = max(orders) + 1
    off = 1084
    patterns = []
    for _ in range(n_pat):
        pat = []
        for j in range(64 * 4):
            cell = d[off:off + 4]
            off += 4
            # Share the decoder with the writer so a mismatch in the cell
            # layout cannot go unnoticed by the verification step.
            pat.append(mod_cell_decode(cell))
        patterns.append(pat)
    samples = []
    for i in range(n_samples):
        h = d[20 + i * 30:20 + (i + 1) * 30]
        if not any(h):
            samples.append(None)
            continue
        wlen = struct.unpack(">H", h[22:24])[0]
        vol = h[25]
        lstart = struct.unpack(">H", h[26:28])[0] * 2
        llen = struct.unpack(">H", h[28:30])[0] * 2
        raw = d[off:off + wlen * 2]
        off += wlen * 2
        vals = [((b + 128) & 0xFF) - 128 for b in raw]
        samples.append(dict(vol=vol, loop_start=lstart, loop_len=llen, data=vals))
    return dict(samples=samples, patterns=patterns, orders=orders, data=d)


def render_mod(mod, out_rate=PREVIEW_RATE):
    """Render a module to a mono float buffer using maxmod's pitch rule.

    Only the effects used by this generator are honoured: Fxx (set speed),
    Bxx (position jump) and Dxx (pattern break).  Notes are held until the
    channel is re-triggered, with a few ms of fade in/out so the picture has
    clean note edges.
    """
    out = []
    step_of = lambda per: (MOD_BASE_RATE * MOD_PERIOD_C / per) / out_rate
    speed = 6
    voices = [None] * 4
    fade = max(2, int(0.004 * out_rate))

    pos_index = 0
    while pos_index < len(mod["orders"]):
        order = mod["orders"][pos_index]
        pattern = mod["patterns"][order]
        row = 0
        while row < 64:
            cells = pattern[row * 4:row * 4 + 4]
            # effects first
            for ch, (snum, period, eff, param) in enumerate(cells):
                if eff == 0x0F and param:
                    if param < 0x20:
                        speed = param
            # notes
            for ch, (snum, period, eff, param) in enumerate(cells):
                if snum and period:
                    smp = mod["samples"][snum - 1]
                    if smp and smp["data"]:
                        voices[ch] = dict(smp=smp, pos=0.0,
                                          step=step_of(period),
                                          gain=smp["vol"] / 64.0, tail=0)
            # render one row's worth of samples
            row_len = int(round(out_rate * speed / TICK_RATE))
            row_buf = [0.0] * row_len
            for ch, v in enumerate(voices):
                if v is None:
                    continue
                data = v["smp"]["data"]
                n = len(data)
                ls, ll = v["smp"]["loop_start"], v["smp"]["loop_len"]
                loop = ll > 2
                pos, step, gain = v["pos"], v["step"], v["gain"]
                tail = v["tail"]
                for i in range(row_len):
                    if pos >= n:
                        if loop:
                            pos -= ll
                        else:
                            v = None
                            break
                    i0 = int(pos)
                    i1 = i0 + 1
                    if i1 >= n:
                        i1 = ls if loop else i0
                    fr = pos - i0
                    s = (data[i0] * (1.0 - fr) + data[i1] * fr) * gain
                    if tail < fade:
                        s *= tail / fade
                        tail += 1
                    row_buf[i] += s
                if v is not None:
                    v["pos"], v["tail"] = pos + step * row_len, tail
            out += row_buf
            jump = None
            for ch, (snum, period, eff, param) in enumerate(cells):
                if eff == 0x0B:
                    jump = param
                elif eff == 0x0D:
                    jump = -1
            row += 1
            if jump is not None:
                break
        if jump is not None:
            if jump == -1:
                pos_index += 1
                jump = None
            else:
                pos_index = jump
                jump = None
            continue
        pos_index += 1
    scale = 1.0 / 128.0 / 4.0
    return [v * scale for v in out]


# --------------------------------------------------------------------------
# PNG preview
# --------------------------------------------------------------------------

def render_preview_png(path, mod_path, song_mod):
    """Waveform + RMS envelope + melody piano-roll, drawn with Pillow."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("  ! Pillow unavailable, skipping preview PNG")
        return None

    mod = read_mod(mod_path)
    wave_buf = render_mod(mod, PREVIEW_RATE)
    secs = len(wave_buf) / float(PREVIEW_RATE)

    W, H = 1280, 840
    img = Image.new("RGB", (W, H), (16, 20, 26))
    dr = ImageDraw.Draw(img)
    dr.text((24, 14), "Capybara Kingdom - birthday.mod preview  "
                      "(rendered from the module bytes at %d Hz, %.1f s loop)"
                      % (PREVIEW_RATE, secs), fill=(226, 232, 240))

    L, R = 40, W - 40

    # ---- panel 1: waveform ------------------------------------------------
    top, bot = 60, 420
    mid = (top + bot) / 2.0
    half = (bot - top) / 2.0 - 4
    dr.rectangle([L, top, R, bot], outline=(48, 56, 68))
    dr.line([L, mid, R, mid], fill=(44, 52, 64))
    n = len(wave_buf)
    for x in range(L, R + 1):
        a = int((x - L) / float(R - L) * n)
        b = int((x - L + 1) / float(R - L) * n)
        if b <= a:
            b = a + 1
        lo, hi = 1e9, -1e9
        for i in range(a, min(b, n)):
            v = wave_buf[i]
            if v < lo:
                lo = v
            if v > hi:
                hi = v
        if lo > hi:
            continue
        y0 = mid - hi * half
        y1 = mid - lo * half
        dr.line([x, min(y0, y1), x, max(y0, y1) + 1], fill=(126, 231, 135))

    # ---- panel 2: RMS envelope -------------------------------------------
    etop, ebot = 448, 548
    dr.rectangle([L, etop, R, ebot], outline=(48, 56, 68))
    cols = R - L + 1
    rms = []
    for x in range(cols):
        a = int(x / float(cols) * n)
        b = int((x + 1) / float(cols) * n)
        s = 0.0
        c = 0
        for i in range(a, min(max(b, a + 1), n)):
            s += wave_buf[i] * wave_buf[i]
            c += 1
        rms.append(math.sqrt(s / c) if c else 0.0)
    peak_rms = max(rms) or 1.0
    for x, v in enumerate(rms):
        hgt = int(v / peak_rms * (ebot - etop - 8))
        if hgt > 0:
            dr.line([L + x, ebot - 3 - hgt, L + x, ebot - 3], fill=(247, 178, 103))
    dr.text((L + 6, etop + 4), "RMS envelope (per column) - no flat silence: "
                               "note onsets and rests are both visible",
            fill=(200, 210, 220))

    # ---- panel 3: melody piano roll --------------------------------------
    ptop, pbot = 580, 800
    dr.rectangle([L, ptop, R, pbot], outline=(48, 56, 68))
    mel = song_mod["patterns"][0].events_of(0)
    tail = song_mod["patterns"][1].events_of(0)
    notes = [(r, p) for (r, s, p) in mel] + [(64 + r, p) for (r, s, p) in tail]
    notes.sort()
    rows_total = 128.0
    if notes:
        lo_p = min(p for _, p in notes)
        hi_p = max(p for _, p in notes)
        span = max(1.0, math.log(hi_p / float(lo_p), 2))
        for k, (row, period) in enumerate(notes):
            end_row = notes[k + 1][0] if k + 1 < len(notes) else row + 8
            x0 = L + 4 + int(row / rows_total * (R - L - 8))
            x1 = L + 4 + int(end_row / rows_total * (R - L - 8))
            y = pbot - 12 - int(math.log(hi_p / float(period), 2) / span * (pbot - ptop - 30))
            dr.rectangle([x0, y - 5, max(x1 - 2, x0 + 2), y + 5],
                         fill=(137, 180, 250), outline=(180, 210, 255))
        dr.text((L + 6, ptop + 4),
                "Melody as written in the module (bar = row; short-short-long phrases)",
                fill=(200, 210, 220))
        for b in range(0, 129, 24):          # phrase boundaries
            x = L + 4 + int(b / rows_total * (R - L - 8))
            dr.line([x, ptop + 22, x, pbot - 2], fill=(60, 70, 84))

    img.save(path)
    return secs


# --------------------------------------------------------------------------
# verification
# --------------------------------------------------------------------------

def verify_wav(path):
    with wave.open(path, "rb") as w:
        n = w.getnframes()
        ch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        raw = w.readframes(n)
    vals = struct.unpack("<%dh" % n, raw)
    peak = max(max(vals), -min(vals))
    rms = math.sqrt(sum(v * v for v in vals) / float(n))
    clipped = sum(1 for v in vals if abs(v) >= FULL_SCALE)
    return dict(frames=n, secs=n / float(sr), ch=ch, sw=sw, sr=sr,
                peak=peak, rms=rms, clipped=clipped, dc=sum(vals) / float(n))


def main(argv):
    root = argv[1] if len(argv) > 1 else "audio"
    music_dir = os.path.join(root, "music")
    sfx_dir = os.path.join(root, "sfx")
    os.makedirs(music_dir, exist_ok=True)
    os.makedirs(sfx_dir, exist_ok=True)

    print("Capybara Kingdom - audio generator")
    print("output root: %s" % os.path.abspath(root))
    print()

    # ---------------- sound effects --------------------------------------
    print("== sound effects (16-bit mono PCM @ %d Hz) ==" % SFX_RATE)
    sfx_files = []
    problems = []
    for i, (name, fn, peak) in enumerate(SFX_SPECS):
        rng = random.Random(1000 + i)          # deterministic -> idempotent
        buf = fn(rng)
        path = os.path.join(sfx_dir, name + ".wav")
        write_wav(path, buf, peak)
        info = verify_wav(path)
        sfx_files.append((name, path, info, peak))
        if not (0.05 <= info["secs"] <= 0.6):
            problems.append("%s: duration %.3f s outside 0.05-0.6 s" % (name, info["secs"]))
        if info["clipped"]:
            problems.append("%s: %d clipped samples" % (name, info["clipped"]))
        if info["rms"] < 200:
            problems.append("%s: suspiciously silent (rms %.1f)" % (name, info["rms"]))

    print("%-14s %8s %7s %8s %8s %8s %6s  %s" %
          ("name", "frames", "secs", "peak", "peak%", "rms%", "clip", "status"))
    for name, path, info, target in sfx_files:
        pk_pct = 100.0 * info["peak"] / FULL_SCALE
        rms_pct = 100.0 * info["rms"] / FULL_SCALE
        ok = (0.05 <= info["secs"] <= 0.6 and not info["clipped"] and info["rms"] >= 200)
        print("%-14s %8d %7.3f %8d %7.1f%% %7.1f%% %6d  %s" %
              (name, info["frames"], info["secs"], info["peak"], pk_pct, rms_pct,
               info["clipped"], "ok" if ok else "CHECK"))
    print("  target peak range requested: 70-85% of full scale")
    print()

    # ---------------- music ----------------------------------------------
    print("== modules (ProTracker M.K., 4 channels, 8-bit samples) ==")
    rng = random.Random(4242)
    mod_files = []
    song_mods = {}
    for builder in SONG_BUILDERS:
        spec = builder()
        data, samples, pats, orders, speed = realize_song(spec, rng)
        path = os.path.join(music_dir, spec["name"] + ".mod")
        write_mod(path, data)
        rows = sum(64 for _ in orders)
        secs = rows * (speed / TICK_RATE)
        nbytes = sum(len(s.data) for s in samples)
        print("%-10s %8d bytes  speed %d  %d patterns  order %-14s "
              "samples %d (%d B)  loop %.1f s%s" %
              (spec["name"] + ".mod", len(data), speed, len(pats),
               str(orders), len(samples), nbytes, secs,
               "" if spec["name"] in LOOPING_SONGS else "  (one-shot)"))
        mod_files.append((spec["name"], path, len(data)))
        song_mods[spec["name"]] = dict(patterns=pats, samples=samples,
                                       orders=orders, speed=speed)
    print()

    # ---------------- preview PNG ---------------------------------------
    print("== verification render ==")
    bday_path = os.path.join(music_dir, "birthday.mod")
    bday = read_mod(bday_path)
    secs = render_preview_png(os.path.join(root, "preview_waveforms.png"),
                              bday_path, song_mods["birthday"])
    if secs:
        print("  wrote %s (%.2f s of birthday.mod rendered at %d Hz)"
              % (os.path.join(root, "preview_waveforms.png"), secs, PREVIEW_RATE))

    # sanity: notes present on the melody channel, and it is not silent
    mel = [e for e in song_mods["birthday"]["patterns"][0].events_of(0)]
    mel += [e for e in song_mods["birthday"]["patterns"][1].events_of(0)]
    print("  birthday.mod melody notes: %d (chord-tone bass notes: %d)"
          % (len(mel),
             len(song_mods["birthday"]["patterns"][0].events_of(1))))
    print()

    # ---------------- summary -------------------------------------------
    print("== files ==")
    total = 0
    for name, path, info, target in sfx_files:
        sz = os.path.getsize(path)
        total += sz
        print("  %-42s %7d bytes" % (os.path.relpath(path).replace("\\", "/"), sz))
    for name, path, sz in mod_files:
        total += sz
        print("  %-42s %7d bytes" % (os.path.relpath(path).replace("\\", "/"), sz))
    png = os.path.join(root, "preview_waveforms.png")
    if os.path.exists(png):
        sz = os.path.getsize(png)
        total += sz
        print("  %-42s %7d bytes" % (os.path.relpath(png).replace("\\", "/"), sz))
    print("  %-42s %7d bytes" % ("TOTAL (%d files)" % (len(sfx_files) + len(mod_files) + 1), total))
    print()
    if problems:
        print("PROBLEMS:")
        for p in problems:
            print("  - " + p)
        return 1
    print("All %d wav + %d mod files generated and verified." %
          (len(sfx_files), len(mod_files)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
