#!/usr/bin/env python3
"""Dump the element tree of a PS Vita CXML/RCO file (header "RCOF").

Usage: cxml_dump.py file.rco [substring-filter]
"""
import struct
import sys


class Cxml:
    def __init__(self, b):
        assert b[:4] == b"RCOF", "not an RCOF/CXML file"
        self.b = b
        h = struct.unpack_from("<4sI18I", b, 0)
        (self.tree_off, self.tree_size, self.ids_off, self.ids_size, self.idi_off, self.idi_size,
         self.str_off, self.str_size, self.wstr_off, self.wstr_size, self.hash_off, self.hash_size,
         self.ia_off, self.ia_size, self.fa_off, self.fa_size, self.file_off, self.file_size) = h[2:]

    def cstr(self, base, off):
        s = base + off
        e = self.b.index(b"\0", s)
        return self.b[s:e].decode("utf-8", "replace")

    def name(self, off):
        return self.cstr(self.str_off, off)

    def node(self, off):
        b, t = self.b, self.tree_off + off
        tag, nattr, parent, prev, nxt, first, last = struct.unpack_from("<7i", b, t)
        attrs = []
        for i in range(nattr):
            a = t + 28 + i * 16
            aname, atype, v1, v2 = struct.unpack_from("<iiii", b, a)
            attrs.append((self.name(aname), atype, v1, v2))
        return self.name(tag), attrs, first, nxt

    def fmt_attr(self, name, atype, v1, v2):
        b = self.b
        try:
            if atype == 1:
                return "%s=%d" % (name, v1)
            if atype == 2:
                return "%s=%g" % (name, struct.unpack("<f", struct.pack("<i", v1))[0])
            if atype == 3:
                return "%s=%r" % (name, self.cstr(self.str_off, v1))
            if atype == 5:
                return "%s=hash:%08x" % (name, struct.unpack_from("<I", b, self.hash_off + v1)[0] if v1 >= 0 else v1)
            if atype == 6:
                vals = struct.unpack_from("<%di" % v2, b, self.ia_off + v1 * 4)
                return "%s=int%s" % (name, list(vals))
            if atype == 7:
                vals = struct.unpack_from("<%df" % v2, b, self.fa_off + v1 * 4)
                return "%s=float%s" % (name, [round(x, 3) for x in vals])
            if atype == 8:
                return "%s=file@%x+%d" % (name, v1, v2)
            if atype in (9, 10):
                return "%s=id:%r" % (name, self.cstr(self.ids_off, v1 + 4))
            if atype in (11, 12):
                return "%s=idint:%08x" % (name, struct.unpack_from("<I", b, self.idi_off + v1 + 4)[0])
        except Exception as e:  # noqa
            return "%s=<type %d: %s>" % (name, atype, e)
        return "%s=<type %d %d %d>" % (name, atype, v1, v2)

    def walk(self, off=0, depth=0, out=None, path=""):
        while off >= 0:
            tag, attrs, first, nxt = self.node(off)
            line = "  " * depth + "<" + tag + " " + " ".join(self.fmt_attr(*a) for a in attrs) + ">"
            out.append((depth, path + "/" + tag, line))
            if first >= 0:
                self.walk(first, depth + 1, out, path + "/" + tag)
            off = nxt
        return out


def main():
    with open(sys.argv[1], "rb") as f:
        c = Cxml(f.read())
    flt = sys.argv[2] if len(sys.argv) > 2 else None
    for depth, path, line in c.walk(0, 0, []):
        if not flt or flt in path:
            print(line)


if __name__ == "__main__":
    main()
