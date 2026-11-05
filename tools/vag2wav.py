#!/usr/bin/env python3
"""Decode Sony VAG (PS-ADPCM) to 16-bit WAV. Usage: vag2wav.py in.vag out.wav"""
import struct
import sys
import wave

COEF = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def decode(b):
    if b[:4] != b"VAGp":
        raise ValueError("not a VAG")
    size = struct.unpack_from(">I", b, 12)[0]
    rate = struct.unpack_from(">I", b, 16)[0]
    data = b[48:48 + size]
    out = []
    s1 = s2 = 0
    for i in range(0, len(data) - 15, 16):
        pred_shift, flags = data[i], data[i + 1]
        shift, pred = pred_shift & 0x0F, (pred_shift >> 4) & 0x0F
        if flags == 7:
            break
        c0, c1 = COEF[pred] if pred < 5 else (0, 0)
        for j in range(14):
            byte = data[i + 2 + j]
            for nib in (byte & 0x0F, byte >> 4):
                v = (nib << 12) & 0xFFFF
                if v & 0x8000:
                    v -= 0x10000
                v >>= shift
                v += (s1 * c0 + s2 * c1) // 64
                v = max(-32768, min(32767, v))
                out.append(v)
                s2, s1 = s1, v
    return rate, out


def main():
    with open(sys.argv[1], "rb") as f:
        rate, pcm = decode(f.read())
    with wave.open(sys.argv[2], "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(pcm), *pcm))
    print("%s: %d Hz, %.3f s" % (sys.argv[2], rate, len(pcm) / rate))


if __name__ == "__main__":
    main()
