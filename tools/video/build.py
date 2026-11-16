#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Build the EtherQ demo video from real screen recordings.

Inputs (TAKES dir, produced on the Linux host by tools/video/record.sh):
  demo.mp4 + demo_marks.txt   QNX 8.0 session (left) + ecatsim dashboard (right)
  build.mp4 + build_marks.txt engineering proof session (QNX SDP build, smoke test)
Narration: tools/video/narration.json, synthesized locally with Kokoro TTS (am_michael).
Output: EtherQ_Demo.mp4 (1920x1080, 30 fps) + EtherQ_Demo.srt

Run: python3 tools/video/build.py <takes_dir>      (needs ffmpeg + Pillow; TTS_DIR for Kokoro)
"""
import json
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TAKES = os.path.abspath(sys.argv[1])
WORK = os.path.join(ROOT, "build", "video", "work")
OUTDIR = os.path.join(ROOT, "build", "video")
OUT = os.path.join(OUTDIR, "EtherQ_Demo.mp4")
TTS_DIR = os.environ.get("TTS_DIR", "/Volumes/DATA/Working/hackathon/2026/buildhybridai-2026/.tts")
F = "/System/Library/Fonts/Supplemental/Arial.ttf"
FB = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
MONO = "/System/Library/Fonts/Menlo.ttc"
W, H, FPS = 1920, 1080, 30
BAND = 100                      # caption band at the bottom
BG = (15, 18, 22)
ACCENT = (74, 163, 255)
OK = (47, 184, 106)
ERR = (229, 72, 77)
TXT = (231, 235, 240)
MUTED = (140, 150, 163)
os.makedirs(WORK, exist_ok=True)

NARR = json.load(open(os.path.join(HERE, "narration.json")))

# scene -> footage: ("card", builder) or (take, mark_from, mark_to, extra_seconds_before_end)
PLAN = {
    "problem": ("card", "problem"),
    "meet": ("mixed", "meet"),
    "discover": ("demo", "slaveinfo", "axisdemo"),
    "drive": ("demo", "axisdemo", "kill"),
    "kill": ("demo", "kill", "blink"),
    "reliability": ("demo", "blink", "end"),
    "proof": ("proof",),
    "closing": ("card", "closing"),
}
PAD = {"problem": 3.0, "meet": 1.5, "discover": 1.2, "drive": 1.5, "kill": 2.0,
       "reliability": 1.2, "proof": 1.5, "closing": 3.5}
# Highlight boxes (scene-relative seconds, pixel boxes measured on the recorded frames)
HIGHLIGHTS = [
    {"scene": "kill", "t0": 6.5, "t1": 99, "x": 6, "y": 812, "w": 790, "h": 34},       # "6 output bytes zeroed and released"
    {"scene": "kill", "t0": 3.0, "t1": 99, "x": 1362, "y": 280, "w": 286, "h": 38},    # drive state SWITCH ON DISABLED
    {"scene": "kill", "t0": 8.5, "t1": 99, "x": 1374, "y": 150, "w": 512, "h": 40},    # all slaves still OP
]


def run(cmd):
    subprocess.run(cmd, check=True)


def dur(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                         capture_output=True, text=True, check=True).stdout
    return float(out)


def marks(name):
    m = {}
    for line in open(os.path.join(TAKES, f"{name}_marks.txt")):
        t, k = line.split()
        m.setdefault(k, float(t))
    t0 = m["start"]
    return {k: v - t0 for k, v in m.items()}


# ------------------------------------------------------------------ narration
def tts():
    py = os.path.join(TTS_DIR, "venv", "bin", "python")
    scenes = []
    for sc in NARR["scenes"]:
        parts = []
        for i, line in enumerate(sc["lines"]):
            f = os.path.join(ROOT, "build", "video", "vo", f"{sc['id']}_{i}.wav")
            if not os.path.exists(f):
                os.makedirs(os.path.dirname(f), exist_ok=True)
                run([py, os.path.join(TTS_DIR, "tts.py"), "kokoro", "am_michael", line, f, str(NARR["speed"])])
            parts.append((line, f, dur(f)))
        scenes.append((sc["id"], parts))
    return scenes


# ---------------------------------------------------------------------- cards
def font(path, size, index=0):
    return ImageFont.truetype(path, size, index=index)


def centered(d, y, text, fnt, fill):
    w = d.textbbox((0, 0), text, font=fnt)[2]
    d.text(((W - w) // 2, y), text, font=fnt, fill=fill)


def card_problem(path):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    centered(d, 150, "EtherQ", font(FB, 120), TXT)
    centered(d, 300, "Crash-safe EtherCAT for QNX OS 8.0", font(F, 54), ACCENT)
    y = 470
    for t, c in [("A motion application can crash.", TXT),
                 ("Its motor must not keep acting on the last command,", TXT),
                 ("and the rest of the machine must keep running.", TXT)]:
        centered(d, y, t, font(F, 46), c)
        y += 74
    centered(d, 820, "Real QNX 8.0 in QEMU  ·  simulated EtherCAT devices  ·  open source (GPL-3.0)",
             font(F, 30), MUTED)
    img.save(path)


def box(d, xy, label, sub, color, fnt, sfnt):
    x0, y0, x1, y1 = xy
    d.rounded_rectangle(xy, radius=18, fill=(23, 27, 33), outline=color, width=4)
    w = d.textbbox((0, 0), label, font=fnt)[2]
    d.text(((x0 + x1 - w) // 2, y0 + 22), label, font=fnt, fill=TXT)
    for i, s in enumerate(sub):
        w = d.textbbox((0, 0), s, font=sfnt)[2]
        d.text(((x0 + x1 - w) // 2, y0 + 78 + i * 38), s, font=sfnt, fill=MUTED)


def arrow(d, x0, y0, x1, y1, color=ACCENT):
    d.line([(x0, y0), (x1, y1)], fill=color, width=5)
    if x1 > x0:
        d.polygon([(x1, y1), (x1 - 18, y1 - 11), (x1 - 18, y1 + 11)], fill=color)
    else:
        d.polygon([(x1, y1), (x1 - 11, y1 - 18), (x1 + 11, y1 - 18)], fill=color)


def card_arch(path):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    fnt, sfnt = font(FB, 34), font(F, 27)
    centered(d, 60, "How EtherQ works on QNX 8.0", font(FB, 56), TXT)
    box(d, (80, 230, 520, 520), "Client processes", ["axisdemo · ecatctl", "your application", "open / pread / pwrite", "devctl / poll"], ACCENT, fnt, sfnt)
    box(d, (680, 180, 1240, 640), "ecatd", ["resource manager /dev/ecat", "cycle: QNX timer pulse,", "SCHED_FIFO priority", "supervisor: recovery", "byte ownership + safe stop", "SOEM v2.0.1 + QNX port"], OK, fnt, sfnt)
    box(d, (1400, 230, 1840, 520), "EtherCAT segment", ["here: ecatsim", "DI8 · DO8 · servo", "(simulated devices)"], MUTED, fnt, sfnt)
    arrow(d, 520, 375, 680, 375)
    d.text((540, 320), "messages", font=font(F, 24), fill=MUTED)
    arrow(d, 1240, 375, 1400, 375)
    d.text((1252, 320), "/dev/bpf", font=font(F, 24), fill=MUTED)
    d.text((1252, 395), "raw frames", font=font(F, 24), fill=MUTED)
    centered(d, 720, "A client crash closes its connection  →  ecatd zeroes that client's outputs  →  bus stays operational",
             font(F, 32), TXT)
    centered(d, 800, "QNX-specific: resource manager · native message passing · timer pulses · io-sock BPF (multi-packet writes) · procmgr abilities",
             font(F, 26), MUTED)
    img.save(path)


def card_closing(path):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    centered(d, 170, "EtherQ", font(FB, 110), TXT)
    centered(d, 310, "Open-source EtherCAT for QNX OS 8.0  ·  crash-safe outputs by design", font(F, 44), ACCENT)
    lines = ["SOEM v2.0.1 QNX port  ·  /dev/ecat resource manager  ·  ecatsim virtual segment  ·  GPL-3.0-or-later",
             "Verified on QNX 8.0 in QEMU (software emulation) with simulated EtherCAT devices.",
             "Not yet validated on physical EtherCAT hardware. No timing or safety-certification claims.",
             "Next step: validation on real EtherCAT I/O and drives."]
    y = 470
    for t in lines:
        centered(d, y, t, font(F, 32), TXT if y < 600 else MUTED)
        y += 62
    small = font(F, 24)
    centered(d, 880, "Narration: synthetic voice (Kokoro TTS).  EtherCAT® is a registered trademark and patented technology,",
             small, MUTED)
    centered(d, 915, "licensed by Beckhoff Automation GmbH, Germany. QNX is a trademark of BlackBerry Limited. Not affiliated.",
             small, MUTED)
    img.save(path)


def card_clip(png, seconds, out):
    run(["ffmpeg", "-v", "error", "-y", "-loop", "1", "-t", f"{seconds:.3f}", "-i", png,
         "-vf", f"fps={FPS},format=yuv420p,fade=t=in:st=0:d=0.5,fade=t=out:st={seconds - 0.5:.3f}:d=0.5",
         "-c:v", "libx264", "-crf", "17", "-an", out])


# -------------------------------------------------------------------- footage
def footage(take, a, b, seconds, out, label_speed=True, max_speed=2.0):
    """Cut [a, b] of take to exactly `seconds`: speed up (max 2x) or hold the last frame.
    The full-width build take is scaled to 90 % so its last lines stay above the caption band."""
    src = os.path.join(TAKES, f"{take}.mp4")
    span = b - a
    speed = max(1.0, min(max_speed, span / seconds))
    used = min(span, seconds * speed)
    hold = max(0.0, seconds - used / speed)
    vf = [f"setpts=PTS/{speed:.4f}" if speed > 1.001 else "null",
          f"tpad=stop_mode=clone:stop_duration={hold:.3f}",
          f"fps={FPS}", "format=yuv420p"]
    if take == "build":
        vf += ["scale=1728:972:flags=lanczos", "pad=1920:1080:96:0:color=0x0f1216"]
    cmd = ["ffmpeg", "-v", "error", "-y", "-ss", f"{a:.3f}", "-t", f"{used:.3f}", "-i", src]
    if label_speed and speed > 1.15:
        png = os.path.join(WORK, f"speed_{speed:.1f}.png")
        label_png(png, f"{speed:.1f}x speed")
        cmd += ["-i", png, "-filter_complex", f"[0:v]{','.join(vf)}[f];[f][1:v]overlay=0:0[v]", "-map", "[v]"]
    else:
        cmd += ["-vf", ",".join(vf)]
    run(cmd + ["-t", f"{seconds:.3f}", "-c:v", "libx264", "-crf", "17", "-an", out])


def label_png(path, text):
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    fnt = font(FB, 26)
    w = d.textbbox((0, 0), text, font=fnt)[2]
    x, y = W - w - 40, H - BAND - 58
    d.rounded_rectangle([x - 14, y - 8, x + w + 14, y + 38], radius=10, fill=(0, 0, 0, 160))
    d.text((x, y), text, font=fnt, fill=(255, 255, 255, 255))
    img.save(path)


def caption_png(path, text):
    """One caption cue rendered into the bottom band (wrapped to two lines max)."""
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    fnt = font(F, 30)
    words, lines, cur = text.split(), [], ""
    for w_ in words:
        trial = (cur + " " + w_).strip()
        if d.textbbox((0, 0), trial, font=fnt)[2] > W - 360 and cur:
            lines.append(cur)
            cur = w_
        else:
            cur = trial
    lines.append(cur)
    lh = 34
    top = H - BAND + 28                       # below the label row
    y0 = top + (BAND - 28 - lh * len(lines)) // 2
    for i, ln in enumerate(lines):
        w = d.textbbox((0, 0), ln, font=fnt)[2]
        d.text(((W - w) // 2, y0 + i * lh), ln, font=fnt, fill=(240, 235, 231, 255))
    img.save(path)


def concat(parts, out):
    lst = os.path.join(WORK, os.path.basename(out) + ".txt")
    with open(lst, "w") as f:
        for p in parts:
            f.write(f"file '{p}'\n")
    run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", out])


# ------------------------------------------------------------------- overlays
def overlay_png(path):
    """Caption band + permanent honesty labels."""
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, H - BAND, W, H], fill=(8, 10, 14, 235))
    lab = font(FB, 17)
    d.text((24, H - BAND + 6), "REAL QNX 8.0 · QEMU (SOFTWARE EMULATION)", font=lab, fill=OK)
    t = "SIMULATED ETHERCAT DEVICES"
    w = d.textbbox((0, 0), t, font=lab)[2]
    d.text((W - w - 24, H - BAND + 6), t, font=lab, fill=(242, 201, 76))
    img.save(path)


CAPTION_FORMS = {"slash dev slash ecat": "/dev/ecat"}


def written(text):
    for spoken, shown in CAPTION_FORMS.items():
        text = text.replace(spoken, shown)
    return text


def srt_time(t):
    ms = int(round(t * 1000))
    return f"{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d},{ms % 1000:03d}"


def main():
    scenes = tts()
    dm, bm = marks("demo"), marks("build")
    clips, audio, cues, t = [], [], [], 0.0
    for sid, parts in scenes:
        speech = sum(p[2] for p in parts) + 0.35 * (len(parts) - 1)
        seconds = speech + PAD[sid]
        plan = PLAN[sid]
        out = os.path.join(WORK, f"scene_{sid}.mp4")
        if plan[0] == "card":
            png = os.path.join(WORK, f"card_{sid}.png")
            {"problem": card_problem, "closing": card_closing}[sid](png)
            card_clip(png, seconds, out)
        elif plan[0] == "proof":
            first = seconds * 0.55
            footage("build", bm["toolchain"], bm["smoke"] + 3.0, first, os.path.join(WORK, "proof_a.mp4"))
            footage("build", bm["end"] - (seconds - first) - 0.5, bm["end"] - 0.5, seconds - first,
                    os.path.join(WORK, "proof_b.mp4"))
            concat([os.path.join(WORK, "proof_a.mp4"), os.path.join(WORK, "proof_b.mp4")], out)
        elif plan[0] == "mixed":
            png = os.path.join(WORK, "card_arch.png")
            card_arch(png)
            a_len = seconds * 0.6
            card_clip(png, a_len, os.path.join(WORK, "meet_a.mp4"))
            footage("demo", dm["identity"], dm["slaveinfo"], seconds - a_len, os.path.join(WORK, "meet_b.mp4"))
            concat([os.path.join(WORK, "meet_a.mp4"), os.path.join(WORK, "meet_b.mp4")], out)
        else:
            take, ma, mb = plan
            m = dm if take == "demo" else bm
            footage(take, m[ma], m[mb], seconds, out)
        clips.append(out)
        # narration lines with small gaps, starting 0.6 s into the scene
        lt = t + 0.6
        for line, wav, d in parts:
            audio.append((lt, wav))
            cues.append((lt, lt + d, written(line)))
            lt += d + 0.35
        t += seconds

    body = os.path.join(WORK, "body.mp4")
    concat(clips, body)
    total = dur(body)

    # narration track
    inputs, filt = [], []
    for i, (st, wav) in enumerate(audio):
        inputs += ["-i", wav]
        filt.append(f"[{i}:a]adelay={int(st * 1000)}|{int(st * 1000)},aresample=48000[a{i}]")
    filt.append("".join(f"[a{i}]" for i in range(len(audio))) +
                f"amix=inputs={len(audio)}:normalize=0,apad,atrim=0:{total:.3f},loudnorm=I=-16:TP=-1.5:LRA=11[aout]")
    voice = os.path.join(WORK, "voice.wav")
    run(["ffmpeg", "-v", "error", "-y", *inputs, "-filter_complex", ";".join(filt), "-map", "[aout]",
         "-ar", "48000", "-ac", "2", voice])

    # captions
    srt = os.path.join(OUTDIR, "EtherQ_Demo.srt")
    with open(srt, "w") as f:
        for i, (a, b, text) in enumerate(cues, 1):
            f.write(f"{i}\n{srt_time(a)} --> {srt_time(b)}\n{text}\n\n")

    ov = os.path.join(WORK, "overlay.png")
    overlay_png(ov)
    hl = []
    if HIGHLIGHTS:
        starts, acc = {}, 0.0
        for sid, _ in scenes:
            starts[sid] = acc
            acc += dur(os.path.join(WORK, f"scene_{sid}.mp4"))
        for h in HIGHLIGHTS:
            sd = dur(os.path.join(WORK, f"scene_{h['scene']}.mp4"))
            a, b = starts[h["scene"]] + h["t0"], starts[h["scene"]] + min(h["t1"], sd)
            hl.append(f"drawbox=x={h['x']}:y={h['y']}:w={h['w']}:h={h['h']}:color={h.get('color', '0x2fb86a')}@0.95:"
                      f"t=5:enable='between(t,{a:.2f},{b:.2f})'")
    inputs = ["-i", body, "-i", ov, "-i", voice]
    graph = [f"[0:v]{','.join(hl) if hl else 'null'}[h0]", "[h0][1:v]overlay=0:0[c0]"]
    for i, (a, b, text) in enumerate(cues):
        png = os.path.join(WORK, f"cap_{i:02d}.png")
        caption_png(png, text)
        inputs += ["-i", png]
        graph.append(f"[c{i}][{i + 3}:v]overlay=0:0:enable='between(t,{a:.2f},{b:.2f})'[c{i + 1}]")
    run(["ffmpeg", "-v", "error", "-y", *inputs, "-filter_complex", ";".join(graph),
         "-map", f"[c{len(cues)}]", "-map", "2:a", "-c:v", "libx264", "-preset", "slow", "-crf", "18",
         "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", OUT])
    print(f"wrote {OUT} ({dur(OUT):.1f} s) and {srt}")


if __name__ == "__main__":
    main()
