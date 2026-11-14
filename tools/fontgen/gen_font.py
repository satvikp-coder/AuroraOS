#!/usr/bin/env python3
"""tools/fontgen/gen_font.py - build the AuroraOS native bitmap font atlas from Noto Sans Mono (stage G4 of issue #7).

Python standard library only (no FreeType, no Pillow). Reproducible: the same pinned input fonts always produce byte-identical
output; run it twice, or on two interpreters, and compare. See third_party/fonts/NotoSansMono/README.md for the provenance.

  python3 tools/fontgen/gen_font.py --regular NotoSansMono-Regular.ttf --bold NotoSansMono-Bold.ttf --out kernel/gfx/font_noto_mono.c

What it does
  * verifies each input file against the pinned SHA-256 (refuses anything else) and checks the font's own licence strings
    (name IDs 0, 13, 14) for the SIL Open Font License 1.1;
  * parses the TrueType tables it needs (head, maxp, hhea, hmtx, cmap formats 4 and 12, loca, glyf with simple and translated
    composite glyphs); hinting instructions are ignored;
  * rasterizes U+0020..U+007E (printable ASCII) with the NON-ZERO winding rule, 8 sub-scanlines per pixel and exact horizontal span
    coverage, then keeps a pixel when at least half of it is covered: a 1-bit mask, no anti-aliasing (the Tier 0 contract is opaque).
    A glyph that would keep under 60 percent of its covered area (a thin stem straddling a pixel boundary) is re-thresholded as low
    as 25 percent coverage so it cannot vanish;
  * lays glyphs out on an integer advance (the font is monospaced: 600/1000 em) and scales x so the glyph exactly fills that
    cell pitch (at most about 4 percent horizontal distortion), so text columns line up at every size;
  * writes a C source with the bitmaps and a glyph table per (weight, size).
Anything outside U+0020..U+007E is NOT in the atlas: the renderer draws a visible missing-glyph box for it. No Hindi or Gujarati.
"""
import argparse, hashlib, struct, sys

PINNED = {   # NotoSansMono-v2.014 (https://github.com/notofonts/latin-greek-cyrillic/releases/tag/NotoSansMono-v2.014), googlefonts/ttf
    "regular": "74fd536351d0f30a73410e1bd223a0ebf763dd4808eb844aa2aed18c0d6e3c84",
    "bold": "4d063be01a00694bf7183d6799d5611760121dcfd526f8b07280ba79115ce02a",
}
SIZES = (12, 14, 16, 18, 24)
FIRST, LAST = 0x20, 0x7E
SUB = 8   # sub-scanlines per pixel


class Font:
    def __init__(self, data):
        self.d = data
        n = struct.unpack(">H", data[4:6])[0]
        self.tab = {}
        for i in range(n):
            tag, _cs, off, ln = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
            self.tab[tag.decode("latin-1")] = (off, ln)
        for t in ("head", "maxp", "hhea", "hmtx", "cmap", "loca", "glyf", "name"):
            if t not in self.tab:
                raise SystemExit("font lacks table " + t)
        h = self.tab["head"][0]
        self.upm = struct.unpack(">H", data[h + 18:h + 20])[0]
        self.loca_fmt = struct.unpack(">h", data[h + 50:h + 52])[0]
        self.nglyphs = struct.unpack(">H", data[self.tab["maxp"][0] + 4:self.tab["maxp"][0] + 6])[0]
        hh = self.tab["hhea"][0]
        self.ascender, self.descender, self.linegap = struct.unpack(">hhh", data[hh + 4:hh + 10])
        self.nhm = struct.unpack(">H", data[hh + 34:hh + 36])[0]
        self.cmap = self._cmap()

    def names(self):
        off, _ = self.tab["name"]
        d = self.d
        count, so = struct.unpack(">HH", d[off + 2:off + 6])
        out = {}
        for i in range(count):
            pid, eid, lid, nid, ln, o = struct.unpack(">HHHHHH", d[off + 6 + 12 * i:off + 18 + 12 * i])
            raw = d[off + so + o:off + so + o + ln]
            s = raw.decode("utf-16-be", "replace") if pid in (0, 3) else raw.decode("latin-1")
            if (nid not in out) or (pid == 3 and lid == 0x409):
                out[nid] = s
        return out

    def _cmap(self):
        off, _ = self.tab["cmap"]
        d = self.d
        n = struct.unpack(">H", d[off + 2:off + 4])[0]
        best = None
        for i in range(n):
            pid, eid, so = struct.unpack(">HHI", d[off + 4 + 8 * i:off + 12 + 8 * i])
            fmt = struct.unpack(">H", d[off + so:off + so + 2])[0]
            if (pid, eid) in ((3, 10), (0, 4), (0, 6)) and fmt == 12:
                best = (off + so, fmt)
            elif (pid, eid) in ((3, 1), (0, 3)) and fmt == 4 and not best:
                best = (off + so, fmt)
        if not best:
            raise SystemExit("no usable cmap")
        base, fmt = best
        m = {}
        if fmt == 4:
            segx2 = struct.unpack(">H", d[base + 6:base + 8])[0]
            seg = segx2 // 2
            ends = struct.unpack(">%dH" % seg, d[base + 14:base + 14 + segx2])
            starts = struct.unpack(">%dH" % seg, d[base + 16 + segx2:base + 16 + 2 * segx2])
            deltas = struct.unpack(">%dh" % seg, d[base + 16 + 2 * segx2:base + 16 + 3 * segx2])
            ro_base = base + 16 + 3 * segx2
            ros = struct.unpack(">%dH" % seg, d[ro_base:ro_base + segx2])
            for i in range(seg):
                for c in range(starts[i], ends[i] + 1):
                    if c == 0xFFFF:
                        continue
                    if ros[i] == 0:
                        g = (c + deltas[i]) & 0xFFFF
                    else:
                        p = ro_base + 2 * i + ros[i] + 2 * (c - starts[i])
                        g = struct.unpack(">H", d[p:p + 2])[0]
                        if g:
                            g = (g + deltas[i]) & 0xFFFF
                    if g:
                        m[c] = g
        else:
            ng = struct.unpack(">I", d[base + 12:base + 16])[0]
            for i in range(ng):
                s, e, g = struct.unpack(">III", d[base + 16 + 12 * i:base + 28 + 12 * i])
                for c in range(s, e + 1):
                    m[c] = g + (c - s)
        return m

    def advance(self, gid):
        off = self.tab["hmtx"][0]
        i = min(gid, self.nhm - 1)
        return struct.unpack(">H", self.d[off + 4 * i:off + 4 * i + 2])[0]

    def _loca(self, gid):
        off = self.tab["loca"][0]
        if self.loca_fmt == 0:
            a, b = struct.unpack(">HH", self.d[off + 2 * gid:off + 2 * gid + 4])
            return a * 2, b * 2
        return struct.unpack(">II", self.d[off + 4 * gid:off + 4 * gid + 8])

    def contours(self, gid, depth=0):
        """List of contours; each contour is a list of (x, y, on_curve) in font units."""
        a, b = self._loca(gid)
        if a == b:
            return []
        g = self.tab["glyf"][0] + a
        d = self.d
        nc = struct.unpack(">h", d[g:g + 2])[0]
        p = g + 10
        if nc >= 0:
            ends = struct.unpack(">%dH" % nc, d[p:p + 2 * nc]); p += 2 * nc
            il = struct.unpack(">H", d[p:p + 2])[0]; p += 2 + il
            npts = (ends[-1] + 1) if nc else 0
            flags = []
            while len(flags) < npts:
                f = d[p]; p += 1; flags.append(f)
                if f & 8:
                    r = d[p]; p += 1
                    flags.extend([f] * r)
            xs, ys, v = [], [], 0
            for f in flags:
                if f & 2:
                    dx = d[p]; p += 1; v += dx if f & 16 else -dx
                elif not f & 16:
                    v += struct.unpack(">h", d[p:p + 2])[0]; p += 2
                xs.append(v)
            v = 0
            for f in flags:
                if f & 4:
                    dy = d[p]; p += 1; v += dy if f & 32 else -dy
                elif not f & 32:
                    v += struct.unpack(">h", d[p:p + 2])[0]; p += 2
                ys.append(v)
            out, s = [], 0
            for e in ends:
                out.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(s, e + 1)])
                s = e + 1
            return out
        if depth > 4:
            raise SystemExit("composite nesting too deep")
        out = []
        while True:
            fl, gi = struct.unpack(">HH", d[p:p + 4]); p += 4
            if fl & 1:
                a1, a2 = struct.unpack(">hh", d[p:p + 4]); p += 4
            else:
                a1, a2 = struct.unpack(">bb", d[p:p + 2]); p += 2
            if not fl & 2:
                raise SystemExit("composite glyph with point-matching is unsupported")
            if fl & (8 | 0x40 | 0x80):
                raise SystemExit("composite glyph with scaling is unsupported")
            for c in self.contours(gi, depth + 1):
                out.append([(x + a1, y + a2, on) for (x, y, on) in c])
            if not fl & 0x20:
                break
        return out


def flatten(contour):
    """Quadratic TrueType contour -> list of closed polylines of (x, y) floats (font units)."""
    pts = contour
    if not pts:
        return []
    # expand implied on-curve points
    ex = []
    n = len(pts)
    for i in range(n):
        x, y, on = pts[i]
        nx, ny, non = pts[(i + 1) % n]
        ex.append((x, y, on))
        if not on and not non:
            ex.append(((x + nx) / 2.0, (y + ny) / 2.0, True))
    # rotate to start at an on-curve point
    k = next((i for i, p in enumerate(ex) if p[2]), None)
    if k is None:
        x0, y0 = (ex[0][0] + ex[1][0]) / 2.0, (ex[0][1] + ex[1][1]) / 2.0
        ex = [(x0, y0, True)] + ex
    else:
        ex = ex[k:] + ex[:k]
    poly = [(float(ex[0][0]), float(ex[0][1]))]
    i, m = 1, len(ex)
    while i <= m:
        p = ex[i % m]
        if p[2]:
            poly.append((float(p[0]), float(p[1]))); i += 1
        else:
            q = ex[(i + 1) % m]
            x0, y0 = poly[-1]
            for t in range(1, 9):
                u = t / 8.0
                a, b, c = (1 - u) * (1 - u), 2 * u * (1 - u), u * u
                poly.append((a * x0 + b * p[0] + c * q[0], a * y0 + b * p[1] + c * q[1]))
            i += 2
    return [poly]


def rasterize(polys, sx, sy):
    """polys in font units; sx, sy = pixels per font unit. Returns (xmin, ymin_top_up, w, h, rows) in pixel space, y up from the
    baseline; rows[r] is a list of 0/1 for r = 0 at the TOP. Coverage >= 50 percent turns a pixel on."""
    edges = []
    for poly in polys:
        n = len(poly)
        for i in range(n):
            (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
            x0, y0, x1, y1 = x0 * sx, y0 * sy, x1 * sx, y1 * sy
            if y0 == y1:
                continue
            edges.append((x0, y0, x1, y1))
    if not edges:
        return None
    ys = [e[1] for e in edges] + [e[3] for e in edges]
    xs = [e[0] for e in edges] + [e[2] for e in edges]
    import math
    xmin, xmax, ymin, ymax = math.floor(min(xs)), math.ceil(max(xs)), math.floor(min(ys)), math.ceil(max(ys))
    w, h = xmax - xmin, ymax - ymin
    cov = [[0.0] * w for _ in range(h)]
    for row in range(h):                       # row 0 = top
        ytop = ymax - row
        for s in range(SUB):
            y = ytop - (s + 0.5) / SUB
            cross = []
            for (x0, y0, x1, y1) in edges:
                if (y0 <= y < y1) or (y1 <= y < y0):
                    t = (y - y0) / (y1 - y0)
                    cross.append((x0 + t * (x1 - x0), 1 if y1 > y0 else -1))
            cross.sort()
            wind = 0
            for k in range(len(cross) - 1):
                wind += cross[k][1]
                if wind != 0:
                    a, b = cross[k][0] - xmin, cross[k + 1][0] - xmin
                    if b <= a:
                        continue
                    for px in range(max(int(math.floor(a)), 0), min(int(math.ceil(b)), w)):
                        lo, hi = max(a, px), min(b, px + 1)
                        if hi > lo:
                            cov[row][px] += (hi - lo) / SUB
    total = sum(sum(r) for r in cov)

    def mask(t):
        return [[1 if c >= t else 0 for c in r] for r in cov]

    rows = mask(0.5)
    on = sum(sum(r) for r in rows)
    if on < 0.6 * total:
        # A thin stroke that straddles a pixel boundary (the "|" at 14 px) covers under half of each pixel it touches and would vanish.
        # Lower the threshold (never below 0.25) to the value whose pixel count is closest to the covered area; ties keep the higher one.
        best, bestd = rows, abs(on - total)
        for t in (0.45, 0.4, 0.35, 0.3, 0.25):
            r2 = mask(t)
            d = abs(sum(sum(r) for r in r2) - total)
            if d < bestd:
                best, bestd = r2, d
        rows = best
    return xmin, ymax, w, h, rows


def trim(g):
    if g is None:
        return None
    xmin, top, w, h, rows = g
    r0 = next((i for i, r in enumerate(rows) if any(r)), None)
    if r0 is None:
        return None
    r1 = max(i for i, r in enumerate(rows) if any(r))
    c0 = min(min(j for j, v in enumerate(r) if v) for r in rows if any(r))
    c1 = max(max(j for j, v in enumerate(r) if v) for r in rows if any(r))
    rows = [r[c0:c1 + 1] for r in rows[r0:r1 + 1]]
    return xmin + c0, top - r0, c1 - c0 + 1, r1 - r0 + 1, rows


def build_face(font, size):
    scale_y = size / float(font.upm)
    adv_units = {font.advance(font.cmap[c]) for c in range(FIRST, LAST + 1)}
    if len(adv_units) != 1:
        raise SystemExit("font is not monospaced over printable ASCII: %s" % sorted(adv_units))
    au = adv_units.pop()
    advance = int(au * scale_y + 0.5)
    scale_x = advance / float(au)               # glyphs exactly fill the integer cell pitch
    ascent = int(font.ascender * scale_y + 0.5)
    descent = int(-font.descender * scale_y + 0.5)
    gap = int(font.linegap * scale_y + 0.5)
    glyphs, blob = [], bytearray()
    for c in range(FIRST, LAST + 1):
        gid = font.cmap[c]
        polys = []
        for cont in font.contours(gid):
            polys.extend(flatten(cont))
        g = trim(rasterize(polys, scale_x, scale_y)) if polys else None
        if g is None:
            glyphs.append((0, 0, 0, 0, len(blob)))
            continue
        xoff, top, w, h, rows = g
        if not (-128 <= xoff <= 127 and -128 <= top <= 127 and w < 256 and h < 256):
            raise SystemExit("glyph metrics out of range for U+%04X" % c)
        glyphs.append((xoff, top, w, h, len(blob)))
        for r in rows:
            for b0 in range(0, w, 8):
                byte = 0
                for k in range(8):
                    if b0 + k < w and r[b0 + k]:
                        byte |= 0x80 >> k
                blob.append(byte)
    if len(blob) > 0xFFFF:
        raise SystemExit("bitmap blob too large for 16-bit offsets")
    return dict(size=size, advance=advance, ascent=ascent, descent=descent, line=ascent + descent + gap, glyphs=glyphs, bits=bytes(blob))


def emit(faces, meta):
    o = []
    o.append("/*")
    o.append(" * kernel/gfx/font_noto_mono.c - GENERATED FILE, do not edit. Regenerate with tools/fontgen/gen_font.py.")
    o.append(" *")
    o.append(" * A 1-bit bitmap rendering of printable ASCII (U+0020..U+007E) taken from Noto Sans Mono " + meta["version"] + ".")
    o.append(" * Source:   " + meta["url"])
    o.append(" * Regular:  " + meta["sha_regular"] + "  (NotoSansMono-Regular.ttf)")
    o.append(" * Bold:     " + meta["sha_bold"] + "  (NotoSansMono-Bold.ttf)")
    o.append(" * " + meta["copyright"])
    o.append(" * Licence:   SIL Open Font License, Version 1.1 (https://scripts.sil.org/OFL). This file is a MODIFIED VERSION of the font software")
    o.append(" *            (outlines rasterized to bitmaps) and is distributed under the same licence. The licence text is in")
    o.append(" *            third_party/fonts/NotoSansMono/OFL.txt. No Reserved Font Name is declared by the upstream font; this file does not")
    o.append(" *            use the font's name as a product name. The rest of AuroraOS is not covered by the OFL.")
    o.append(" * Layout:   per face, glyph table rows are { xoff, top, w, h, offset }: the bitmap is w x h pixels, one row per ceil(w/8) bytes,")
    o.append(" *            bit 7 = leftmost pixel; its left edge is xoff pixels right of the pen position and its top row is `top` pixels")
    o.append(" *            above the baseline. Blank glyphs (space) have w = h = 0. All glyphs share the integer advance.")
    o.append(" */")
    o.append("#include <gfx_font.h>")
    o.append("")
    names = []
    for (wt, name), f in faces:
        ident = "noto_%s_%d" % (name, f["size"])
        names.append((ident, wt, f))
        o.append("static const uint8_t %s_bits[%d] = {" % (ident, max(len(f["bits"]), 1)))
        bits = f["bits"] or b"\x00"
        for i in range(0, len(bits), 16):
            o.append("    " + ", ".join("0x%02x" % b for b in bits[i:i + 16]) + ",")
        o.append("};")
        o.append("static const struct gfx_glyph %s_glyphs[%d] = {" % (ident, LAST - FIRST + 1))
        for i in range(0, len(f["glyphs"]), 4):
            o.append("    " + " ".join("{ %d, %d, %d, %d, %d }," % g for g in f["glyphs"][i:i + 4]))
        o.append("};")
        o.append("")
    for ident, wt, f in names:
        o.append("static const struct gfx_font %s = { \"Noto Sans Mono\", %d, %d, %d, %d, %d, %d, %d, %d, %s_glyphs, %s_bits, %d };" % (
            ident, wt, f["size"], f["advance"], f["ascent"], f["descent"], f["line"], FIRST, LAST, ident, ident, len(f["bits"]) or 1))
    o.append("")
    o.append("const struct gfx_font *const gfx_fonts[] = {")
    for ident, wt, f in names:
        o.append("    &%s," % ident)
    o.append("};")
    o.append("const int gfx_font_count = %d;" % len(names))
    return "\n".join(o) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--regular", required=True)
    ap.add_argument("--bold", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    fonts = {}
    for key, path in (("regular", a.regular), ("bold", a.bold)):
        data = open(path, "rb").read()
        sha = hashlib.sha256(data).hexdigest()
        if sha != PINNED[key]:
            raise SystemExit("%s font has SHA-256 %s, expected the pinned %s" % (key, sha, PINNED[key]))
        f = Font(data)
        nm = f.names()
        lic = (nm.get(13, "") + " " + nm.get(14, "")).lower()
        if "open font license" not in lic or "1.1" not in lic:
            raise SystemExit("font licence strings do not say SIL OFL 1.1: %r" % nm.get(13))
        fonts[key] = (f, nm)
    nm = fonts["regular"][1]
    meta = dict(version="v2.014", url="https://github.com/notofonts/latin-greek-cyrillic/releases/tag/NotoSansMono-v2.014 (googlefonts/ttf)",
                sha_regular=PINNED["regular"], sha_bold=PINNED["bold"], copyright=nm.get(0, "").replace("\n", " "))
    faces = []
    for wt, key, name in ((400, "regular", "regular"), (700, "bold", "bold")):
        for size in SIZES:
            faces.append(((wt, name), build_face(fonts[key][0], size)))
    text = emit(faces, meta)
    with open(a.out, "w", newline="\n") as fh:
        fh.write(text)
    print("wrote %s: %d faces, %d bytes of bitmap, SHA-256 of the output %s" % (a.out, len(faces), sum(len(f["bits"]) for _, f in faces), hashlib.sha256(text.encode()).hexdigest()))


if __name__ == "__main__":
    sys.exit(main())
