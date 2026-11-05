#!/usr/bin/env python3
"""Find embedded audio inside PS Vita firmware resource files (.rco / CXML).

Usage: rco_scan.py <vs0 dir> [--dump OUTDIR]

Vita RCOs are CXML containers ("RCOF"); their payload may be zlib-compressed.
We inflate every zlib stream we can find and look for audio signatures in both
the raw and inflated bytes. Nothing is executed from the scanned files.
"""
import os
import struct
import sys
import zlib

SIGS = {
    b"RIFF": "riff",
    b"VAGp": "vag",
    b"OggS": "ogg",
    b"MSFC": "msf",
    b"MSF\x43": "msf",
    b"SSCF": "sscf",
}


def riff_info(b, off):
    if b[off + 8:off + 12] != b"WAVE":
        return None
    size = struct.unpack_from("<I", b, off + 4)[0] + 8
    fmt = None
    p = off + 12
    while p + 8 <= min(len(b), off + size):
        cid = b[p:p + 4]
        clen = struct.unpack_from("<I", b, p + 4)[0]
        if cid == b"fmt " and clen >= 16:
            tag, ch, rate = struct.unpack_from("<HHI", b, p + 8)
            bits = struct.unpack_from("<H", b, p + 22)[0]
            if tag == 0xFFFE and clen >= 40:
                guid = b[p + 32:p + 48]
                if guid[:4] == b"\xd2\x42\xe1\x47":
                    tag = "ATRAC9"
            fmt = (tag, ch, rate, bits)
        p += 8 + clen + (clen & 1)
    return size, fmt


def inflate_all(b):
    """Yield (offset, data) for every zlib stream that inflates to something sizeable."""
    seen = 0
    i = 0
    while i < len(b) - 2:
        if b[i] == 0x78 and b[i + 1] in (0x01, 0x5E, 0x9C, 0xDA):
            try:
                d = zlib.decompressobj()
                out = d.decompress(b[i:i + 64 * 1024 * 1024])
                if len(out) > 256:
                    yield i, out
                    consumed = len(b[i:]) - len(d.unused_data)
                    i += max(consumed, 2)
                    seen += 1
                    continue
            except zlib.error:
                pass
        i += 1


def scan(data):
    hits = []
    for sig, kind in SIGS.items():
        start = 0
        while True:
            o = data.find(sig, start)
            if o < 0:
                break
            start = o + 1
            if kind == "riff":
                info = riff_info(data, o)
                if not info:
                    continue
                hits.append((o, kind, info[0], info[1]))
            elif kind == "vag":
                size = struct.unpack_from(">I", data, o + 12)[0] + 48
                rate = struct.unpack_from(">I", data, o + 16)[0]
                name = data[o + 32:o + 48].split(b"\0")[0].decode("latin1", "replace")
                hits.append((o, kind, size, ("VAG", 1, rate, name)))
            else:
                hits.append((o, kind, 0, None))
    return sorted(hits)


def main():
    root = sys.argv[1]
    dump = None
    if "--dump" in sys.argv:
        dump = sys.argv[sys.argv.index("--dump") + 1]
        os.makedirs(dump, exist_ok=True)
    total = 0
    for dp, _, files in os.walk(root):
        for fn in sorted(files):
            path = os.path.join(dp, fn)
            if os.path.getsize(path) > 64 * 1024 * 1024:
                continue
            with open(path, "rb") as f:
                b = f.read()
            layers = [("raw", 0, b)]
            if fn.endswith((".rco", ".cxml", ".dat")) or b[:4] == b"RCOF":
                for off, out in inflate_all(b):
                    layers.append(("zlib@%x" % off, off, out))
            for lname, loff, data in layers:
                for o, kind, size, info in scan(data):
                    total += 1
                    rel = os.path.relpath(path, root)
                    print("%-60s %-12s +%-8x %-5s size=%-7d %s" % (rel, lname, o, kind, size, info))
                    if dump and size:
                        ext = "at9" if info and info[0] == "ATRAC9" else ("vag" if kind == "vag" else "wav")
                        safe = rel.replace("/", "_").replace(":", "")
                        with open(os.path.join(dump, "%s_%s_%x.%s" % (safe, lname.replace("@", ""), o, ext)), "wb") as out:
                            out.write(data[o:o + size])
    print("total audio hits:", total, file=sys.stderr)


if __name__ == "__main__":
    main()
