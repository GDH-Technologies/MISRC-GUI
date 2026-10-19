"""CVBS verification helper: per-row horizontal position of the white bar's
left/right edge, the first yellow pixel and the row mean luma in a P6 PPM frame
dump (e.g. gui_cvbs_test.exe --panel --full --ppm=out.ppm). Used to prove that
the first picture rows after the VBI sit at the same x as mid-frame rows.
Usage: python cvbs_row_edges.py out.ppm [row ...]"""
import sys

def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    # P6\nW H\n255\n
    parts = []
    pos = 0
    while len(parts) < 4:
        while data[pos:pos+1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos+1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1  # single whitespace after maxval
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:pos + w * h * 3]

def main():
    path = sys.argv[1]
    rows = [int(a) for a in sys.argv[2:]] if len(sys.argv) > 2 else None
    w, h, px = read_ppm(path)
    print(f"{path}: {w}x{h}")
    if rows is None:
        rows = list(range(0, 80)) + list(range(300, 306)) + list(range(560, 625, 8))
    print("row  white_left  white_right  yellow_left  mean_luma  bright_px")
    for y in rows:
        if y >= h:
            continue
        base = y * w * 3
        wl = wr = yl = -1
        bright = 0
        lum = 0
        for x in range(w):
            r, g, b = px[base + 3*x], px[base + 3*x + 1], px[base + 3*x + 2]
            l = (r * 299 + g * 587 + b * 114) // 1000
            lum += l
            if r > 180 and g > 180 and b > 180:
                bright += 1
                if wl < 0:
                    wl = x
                wr = x
            if yl < 0 and r > 150 and g > 150 and b < 90:
                yl = x
        print(f"{y:4d} {wl:10d} {wr:12d} {yl:12d} {lum // w:10d} {bright:9d}")

main()
