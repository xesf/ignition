"""Fit each row of a level shade table as a premultiplied blend of the palette.

LEVEL_DIR/NAME.COL holds the palette (bytes 8..776, 256 RGB triples, 6-bit
values are scaled to 8-bit when the largest one is below 64). LEVEL_DIR/
NAME.SHD (or NAME.PAN with --table PAN) holds a 64 KB table T[s<<8|d]: for
source colour s and destination colour d, the output colour is palette[T[s<<8|d]].

For each source row s the output colours out[d] = palette[T[s][d]] are fit by

    out[d] ~= C + k * palette[d],   k clamped to [0, 1],  C per channel

which is premultiplied alpha blending with alpha = 1 - k. The fit is exact for
the clamped problem: k is the least-squares slope of the centred data, clamped.

Kind of each row:
    identity     T[s][d] == d for every d
    solid colour N   T[s][d] == N for every d
    blend        not the above, fit RMS < 8
    ramp         not the above, fit RMS >= 8 (no single blend explains it)

Row quality: exact if RMS < 0.5, fit if RMS < 8, poor otherwise.

--counts FILE is an optional text file with 256 whitespace-separated integers
(pixel count per source colour). With it, the count-weighted mean RMS is printed
and the 10 rows with the largest count * RMS are listed.

    python3 -I tools/shd_fit.py LEVEL_DIR NAME [--table SHD|PAN] [--counts FILE]
"""
import argparse
import os

import numpy as np

LUM = np.array([0.3, 0.59, 0.11])


def load_palette(path):
    raw = open(path, "rb").read()[8:776]
    if len(raw) != 768:
        raise SystemExit("%s: palette needs 768 bytes after the 8-byte header" % path)
    pal = np.frombuffer(raw, np.uint8).reshape(256, 3).astype(float)
    if pal.max() < 64:
        pal *= 255 / 63
    return pal


def load_table(path):
    raw = open(path, "rb").read()
    if len(raw) != 65536:
        raise SystemExit("%s: expected 65536 bytes, got %d" % (path, len(raw)))
    return np.frombuffer(raw, np.uint8).reshape(256, 256)


def fit_row(out, pal):
    """Return (C[3], k, rms) for out[d] ~= C + k * pal[d], k in [0, 1]."""
    mo = out.mean(0)
    mp = pal.mean(0)
    co = out - mo
    cp = pal - mp
    den = (cp * cp).sum()
    k = float((co * cp).sum() / den) if den > 0 else 0.0
    k = min(1.0, max(0.0, k))
    C = mo - k * mp
    resid = out - C - k * pal
    rms = float(np.sqrt((resid * resid).mean()))
    return C, k, rms


def classify(row_idx, out, pal, rms):
    if (row_idx == np.arange(256)).all() or np.array_equal(out, pal):
        return "identity"
    if (row_idx == row_idx[0]).all():
        return "solid colour %d" % row_idx[0]
    return "blend" if rms < 8 else "ramp"


def read_counts(path):
    vals = open(path).read().split()
    if len(vals) != 256:
        raise SystemExit("%s: expected 256 integers, got %d" % (path, len(vals)))
    return np.array([int(v) for v in vals], dtype=float)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("level_dir")
    ap.add_argument("name")
    ap.add_argument("--table", choices=["SHD", "PAN"], default="SHD")
    ap.add_argument("--counts", metavar="FILE")
    args = ap.parse_args()

    pal = load_palette(os.path.join(args.level_dir, args.name + ".COL"))
    T = load_table(os.path.join(args.level_dir, "%s.%s" % (args.name, args.table)))
    counts = read_counts(args.counts) if args.counts else None

    rms_all = np.zeros(256)
    print("%3s  %-18s %-22s %6s %7s" % ("src", "kind", "C (r,g,b)", "k", "rms"))
    n_exact = n_fit = n_poor = 0
    for s in range(256):
        row = T[s]
        out = pal[row]
        C, k, rms = fit_row(out, pal)
        rms_all[s] = rms
        kind = classify(row, out, pal, rms)
        if rms < 0.5:
            n_exact += 1
        elif rms < 8:
            n_fit += 1
        else:
            n_poor += 1
        print("%3d  %-18s (%6.1f,%6.1f,%6.1f) %6.3f %7.2f"
              % (s, kind, C[0], C[1], C[2], k, rms))

    if counts is not None:
        total = counts.sum()
        wmean = float((counts * rms_all).sum() / total) if total > 0 else 0.0
        print()
        print("count-weighted mean RMS: %.2f (over %d pixels)" % (wmean, int(total)))
        weighted = counts * rms_all
        worst = np.argsort(-weighted, kind="stable")[:10]
        print("10 worst rows by count * RMS:")
        print("%3s  %10s %7s %12s" % ("src", "count", "rms", "weighted"))
        for s in worst:
            print("%3d  %10d %7.2f %12.1f" % (s, int(counts[s]), rms_all[s], weighted[s]))

    print()
    print("rows exact/fit(<8)/poor: %d/%d/%d" % (n_exact, n_fit, n_poor))


if __name__ == "__main__":
    main()
