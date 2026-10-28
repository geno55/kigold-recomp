"""Check a KIG_FRAME_LOG for screen updates that show a buffer mid-frame or out of order.

A frame is drawn into a buffer as an RDP list (R) and then a display list (D). Problems reported:
  mid-frame   a screen update (S) of buffer X between X's R and its D: only the clear/background is drawn
  flip-back   a screen update showing an older frame than the previous screen update

Usage: python tools/frame_order.py build/run/frames.log [--from T] [--to T]
"""
import argparse

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("--from", dest="t0", type=float, default=0)
ap.add_argument("--to", dest="t1", type=float, default=1e9)
a = ap.parse_args()

pending_r = {}      # buffer -> time of an R whose D hasn't come yet
last_complete = {}  # buffer -> frame number of its last complete (R+D) frame
frame = 0
prev_shown = -1
mid, back, vis = [], [], 0


def buf_of(origin):
    cands = [b for b in set(pending_r) | set(last_complete) if b <= origin < b + 0x1000]
    return max(cands) if cands else None


for line in open(a.log):
    p = line.split()
    kind, t = p[0], float(p[1])
    if kind == "R" and p[3] != "000000":
        pending_r[int(p[3], 16)] = t
    elif kind == "D" and p[3] != "000000":
        b = int(p[3], 16)
        pending_r.pop(b, None)
        frame += 1
        last_complete[b] = frame
    elif kind == "S":
        if not (a.t0 <= t <= a.t1):
            continue
        vis += 1
        b = buf_of(int(p[2], 16))
        if b in pending_r:
            mid.append((t, b))
        f = last_complete.get(b, -1)
        if 0 <= f < prev_shown:
            back.append((t, prev_shown, f))
        prev_shown = max(prev_shown, f) if f >= 0 else prev_shown
print(f"{vis} screen updates; mid-frame: {len(mid)}; flip-backs: {len(back)}")
for t, b in mid[:15]:
    print(f"  mid-frame at {t:.4f}: buffer {b:06X}")
for t, x, y in back[:15]:
    print(f"  flip-back at {t:.4f}: frame {y} after {x}")
