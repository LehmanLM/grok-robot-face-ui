#!/usr/bin/env python3
"""合成 robot-ui 待机卡通音效（非人声，纯正弦/噪声合成）。

注意：线上音效已换用 Kenney CC0 精选（interface-sounds + digital-audio，
ffmpeg 转 24k mono、volume=0.65），本脚本仅作离线合成兜底：
  blip=question_003  sparkle=confirmation_002
  boing=pluck_001    swoosh=phaserUp2
规格：24kHz mono s16（与 TTS 链一致）。
板上 aplay 直播；robot-ui fork+execlp 触发（见 robot-ui.c sfx_play）。
"""
import math
import os
import random
import struct
import wave

RATE = 24000
AMP = 0.32
OUT = os.path.join(os.path.dirname(__file__),
                   "..", "..", "..", "meta", "meta-rk3566-robot-assistant",
                   "recipes-robot", "robot-ui", "files", "sfx")


def write_wav(name: str, samples: list[float]):
    path = os.path.join(OUT, name)
    os.makedirs(OUT, exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for s in samples:
            v = max(-1.0, min(1.0, s))
            frames += struct.pack("<h", int(v * 32767))
        w.writeframes(bytes(frames))
    print(f"{name}: {len(samples) / RATE * 1000:.0f}ms {os.path.getsize(path)}B")


def env_exp(t, dur, k=6.0):
    return math.exp(-k * t / dur)


def blip():
    """换形轻 blip：620→940Hz 上滑音，快起指数衰减。"""
    dur = 0.11
    n = int(RATE * dur)
    out = []
    for i in range(n):
        t = i / RATE
        p = t / dur
        f = 620 + 320 * p
        e = math.exp(-9 * p) * min(1, i / (RATE * 0.004))
        out.append(AMP * 0.8 * e * math.sin(2 * math.pi * f * t))
    return out


def swoosh():
    """转圈嗖嗖：环调噪声扫频（180→850Hz），鼓包包络。"""
    dur = 0.48
    n = int(RATE * dur)
    rnd = random.Random(20261018)
    out = []
    ph = 0.0
    for i in range(n):
        t = i / RATE
        p = t / dur
        f = 180 + 670 * p * p
        ph += 2 * math.pi * f / RATE
        e = math.sin(math.pi * min(1, p * 1.05)) ** 0.6
        noise = rnd.random() * 2 - 1
        carrier = math.sin(ph)
        out.append(AMP * 0.55 * e * noise * 0.35 * abs(carrier)
                   + AMP * 0.25 * e * carrier * 0.3)
    return out


def sparkle():
    """粒子爆发 sparkle：五声音阶上行纯音 + 微失谐闪光。"""
    notes = [880, 1046.5, 1318.5, 1568, 2093]
    dur = 0.5
    n = int(RATE * dur)
    out = [0.0] * n
    for k, f in enumerate(notes):
        start = int(RATE * 0.075 * k)
        tone_dur = 0.22
        m = int(RATE * tone_dur)
        for i in range(m):
            if start + i >= n:
                break
            t = i / RATE
            p = i / m
            e = math.exp(-7 * p) * min(1, i / (RATE * 0.003))
            s = (math.sin(2 * math.pi * f * t)
                 + 0.35 * math.sin(2 * math.pi * f * 1.006 * t))
            out[start + i] += AMP * 0.5 * e * s / len(notes)
    return out


def boing():
    """唤醒 boing：低频弹簧颤音（vibrato 深度衰减 + 上滑起音）。"""
    dur = 0.42
    n = int(RATE * dur)
    out = []
    ph = 0.0
    for i in range(n):
        t = i / RATE
        p = t / dur
        depth = 0.55 * (1 - p) + 0.04
        f = 195 * (1 + 0.22 * math.exp(-8 * p)) * (1 + depth * math.sin(2 * math.pi * 11 * t))
        ph += 2 * math.pi * f / RATE
        e = min(1, i / (RATE * 0.006)) * math.exp(-5 * p)
        out.append(AMP * e * (math.sin(ph) + 0.22 * math.sin(2 * ph)))
    return out


if __name__ == "__main__":
    write_wav("blip.wav", blip())
    write_wav("swoosh.wav", swoosh())
    write_wav("sparkle.wav", sparkle())
    write_wav("boing.wav", boing())
