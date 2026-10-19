"""CVBS verification helper: raw-signal probe around one vertical interval of a
40 MSPS s16 capture. Prints the pulse list after the first pre-EQ (position in
lines, width in us, kind E/H/B at the steady-state threshold) and per-line levels
(front porch, tip window, back porch, active, min/max) on the nominal line grid -
the measurement that exposed the post-VBI baseline wander (2026-10-19).
Usage: python cvbs_vbi_probe.py capture_40msps_s16.raw [start_seconds]
Note: the per-line grid assumes PAL parity-0 (first EQ half a line after the
last H); on a parity-1 field the level columns are half a line off - read the
pulse list and the min/max columns, which are grid-independent."""
import sys, struct, array

path = sys.argv[1]
rate = 40_000_000
start_s = float(sys.argv[2]) if len(sys.argv) > 2 else 0.30
nsamp = int(0.10 * rate)  # 100 ms window = ~5 fields
with open(path, 'rb') as f:
    f.seek(int(start_s * rate) * 2)
    raw = f.read(nsamp * 2)
a = array.array('h')
a.frombytes(raw)
n = len(a)

# Coarse levels from a histogram-free approach: tip = 1st percentile, blank ~ mode region.
srt = sorted(a[:2_000_000:7])
tip = srt[len(srt) // 100]
white = srt[len(srt) * 99 // 100]
# Blank estimate: median of samples between tip+25% and tip+45% of (white-tip) is unreliable;
# use the sync detection with threshold = tip + 0.5*(blank-tip) iteratively: first guess blank = tip + 0.29*(white-tip)
blank = tip + int(0.29 * (white - tip))
thr = (tip + blank) // 2
L = 64e-6 * rate  # 2560 samples

# Find low pulses on a 500 kHz-ish 1-pole LPF of the signal (alpha 0.08 like the decoder scaled to 40 MSPS ~ tau 12 samples)
pulses = []
lp = float(a[0])
alpha = 0.08
in_p = False
p0 = 0
for i in range(n):
    lp += alpha * (a[i] - lp)
    if not in_p and lp < thr:
        in_p = True; p0 = i
    elif in_p and lp >= thr:
        in_p = False
        pulses.append((p0, i - p0))

def kind(w):
    us = w / rate * 1e6
    if 0.6 <= us < 3.5: return 'E'
    if 3.5 <= us <= 8.0: return 'H'
    if 19.0 <= us <= 36.0: return 'B'
    return '?'

# Locate first broad-pulse run
first_b = None
for idx, (p, w) in enumerate(pulses):
    if kind(w) == 'B':
        first_b = idx; break
if first_b is None:
    print("no broad pulse found"); sys.exit(1)
# back up to the first EQ of the pre-train
j = first_b
while j > 0 and kind(pulses[j-1][1]) == 'E':
    j -= 1
first_eq = pulses[j][0]
print(f"tip~{tip} blank~{blank} white~{white} thr={thr}; first pre-EQ at sample {first_eq} (t={start_s + first_eq/rate:.4f}s)")
print("pulses from first EQ (pos_rel_lines width_us kind):")
for (p, w) in pulses[j:j+60]:
    print(f"  {(p-first_eq)/L:7.3f}  {w/rate*1e6:6.2f} {kind(w)}")

# Per-line analysis on the nominal grid: line k starts at first_eq + (k-0.5)*L (PAL parity-0 convention)
print("\nline  edge_err(smp)  front(-1.0us)  tip(2us)   back(9us)   active(30us)  min   max")
for k in range(0, 60):
    s = int(first_eq + (k - 0.5) * L)
    if s < 100 or s + int(L) >= n: continue
    def mean(t0, t1):
        i0 = s + int(t0 * 1e-6 * rate); i1 = s + int(t1 * 1e-6 * rate)
        seg = a[i0:i1]
        return sum(seg) / max(1, len(seg))
    # locate the actual falling edge near s on the LPF'd signal: search +-200 samples for first crossing below thr
    lp = float(a[s - 400])
    edge = None
    for i in range(s - 399, s + 400):
        prev = lp
        lp += alpha * (a[i] - lp)
        if prev >= thr and lp < thr:
            edge = i; break
    edge_err = (edge - s) if edge is not None else float('nan')
    seg = a[s:s + int(L)]
    print(f"{k:4d}  {edge_err!s:>12}  {mean(-1.5,-0.3):12.0f}  {mean(1.5,3.5):9.0f}  {mean(8.0,10.0):9.0f}  {mean(28,32):12.0f}  {min(seg):6d} {max(seg):6d}")
