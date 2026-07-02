#!/usr/bin/env python3
"""
gen_demo_song.py  -  generate tOS original demo melody as WAV + C embed
Produces: kernel/audio/demo_song.wav  and  kernel/audio/demo_song.h

Melody: "tOS Jingle" - original composition, no copyright, chiptune style.
        Square-wave, 8-bit unsigned, 22050 Hz, mono.
"""

import struct, math, os

RATE   = 22050   # Hz
BITS   = 8       # unsigned 8-bit
CH     = 1
BPM    = 138

BEAT   = RATE * 60 // BPM          # samples per quarter note
Q      = BEAT
HALF   = BEAT // 2
THIRD  = BEAT // 3

# ── Frequency helpers ────────────────────────────────────────────────────────
def hz(semitones_from_C4):
    return 261.63 * (2 ** (semitones_from_C4 / 12.0))

C4=hz(0); D4=hz(2); E4=hz(4); F4=hz(5); G4=hz(7); A4=hz(9); B4=hz(11)
C5=hz(12);D5=hz(14);E5=hz(16);F5=hz(17);G5=hz(19);A5=hz(21)
G3=hz(-5); F3=hz(-7); E3=hz(-8); C3=hz(-12)

REST = 0.0

# ── Synthesis ────────────────────────────────────────────────────────────────
def render_note(freq, dur, amp=72):
    """Square wave with 5ms attack+decay envelope to reduce clicks."""
    samples = []
    fade = min(int(RATE * 0.005), dur // 8)
    for i in range(dur):
        if freq == REST:
            samples.append(128)
            continue
        env = 1.0
        if i < fade:
            env = i / fade
        elif i > dur - fade:
            env = (dur - i) / fade
        phase = (i * freq / RATE) % 1.0
        val = amp if phase < 0.5 else -amp
        samples.append(int(128 + val * env))
    return samples

# ── Melody (original, 4-bar phrases × 4 repetitions) ────────────────────────
# Phrase A  – ascending arpeggio theme
A = [
    (C4, Q),(E4, Q),(G4, Q),(C5, Q),
    (A4, Q),(C5, Q),(E5, Q),(A4, Q),
    (F4, Q),(A4, Q),(C5, Q),(F4, Q),
    (G4, Q),(B4, Q),(D5, Q),(G4, BEAT+HALF),
]
# Phrase B  – stepwise answer
B = [
    (E5, Q),(D5, HALF),(C5, Q),(D5, HALF),
    (E5, Q),(E5, Q),(E5, BEAT+Q),
    (D5, Q),(D5, Q),(D5, BEAT+Q),
    (E5, Q),(G5, BEAT+HALF),
]
# Phrase C  – descending echo
C_ = [
    (C5, Q),(G4, Q),(E4, Q),(C4, Q),
    (F4, Q),(C5, Q),(A4, Q),(F4, Q),
    (G4, Q),(D5, Q),(B4, Q),(G4, Q),
    (C5, BEAT*3),
]
# Phrase D  – energetic bridge
D = [
    (G5, HALF),(F5, HALF),(E5, HALF),(D5, HALF),
    (C5, Q),(D5, Q),(E5, Q),(F5, Q),
    (G5, BEAT+Q),(REST, HALF),
    (C5, Q),(E5, Q),(G5, Q),(A5, BEAT+Q),
]

# Full song: A B A C D A (≈30 seconds at BPM 138)
song = A + B + A + C_ + D + A

# ── Render ────────────────────────────────────────────────────────────────────
samples = []
for freq, dur in song:
    samples.extend(render_note(freq, dur))

print(f"  Song: {len(samples)} samples, {len(samples)/RATE:.1f} seconds")

# ── WAV file ─────────────────────────────────────────────────────────────────
os.makedirs("kernel/audio", exist_ok=True)
wav_path = "kernel/audio/demo_song.wav"
with open(wav_path, "wb") as f:
    data = bytes(samples)
    dsize = len(data)
    f.write(b"RIFF")
    f.write(struct.pack("<I", 36 + dsize))
    f.write(b"WAVE")
    f.write(b"fmt ")
    f.write(struct.pack("<I", 16))
    f.write(struct.pack("<H", 1))     # PCM
    f.write(struct.pack("<H", CH))
    f.write(struct.pack("<I", RATE))
    f.write(struct.pack("<I", RATE * CH))  # byte rate
    f.write(struct.pack("<H", CH))         # block align
    f.write(struct.pack("<H", BITS))
    f.write(b"data")
    f.write(struct.pack("<I", dsize))
    f.write(data)
print(f"  WAV written: {wav_path} ({os.path.getsize(wav_path)//1024} KB)")

# ── C embed header ────────────────────────────────────────────────────────────
hdr_path = "kernel/audio/demo_song.h"
with open(hdr_path, "w") as f:
    f.write(
        "#ifndef DEMO_SONG_H\n"
        "#define DEMO_SONG_H\n"
        "#include <stdint.h>\n\n"
        "/* tOS original demo melody - auto-generated, do not edit */\n"
        f"#define DEMO_SONG_RATE   {RATE}\n"
        f"#define DEMO_SONG_BITS   {BITS}\n"
        f"#define DEMO_SONG_CH     {CH}\n"
        f"#define DEMO_SONG_LEN    {len(samples)}U\n\n"
        "extern const uint8_t demo_song_pcm[DEMO_SONG_LEN];\n\n"
        "#endif\n"
    )
print(f"  Header written: {hdr_path}")
