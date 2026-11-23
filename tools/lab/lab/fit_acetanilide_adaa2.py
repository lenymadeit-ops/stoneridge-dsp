"""Fit the odd restoring torque of the acetanilide PES and print ADAA2 coefficients."""
import json
import math
from pathlib import Path

pes_path = Path(r"C:\Users\dell\.gemini\antigravity\brain\5d1b170b-c675-4762-bfce-ba4960c2d527\scratch\acetanilide_pes.json")
if not pes_path.exists():
    raise SystemExit(f"missing {pes_path}")

pes = json.loads(pes_path.read_text(encoding="utf-8"))
angles = pes["angles_deg"]
energy = pes["energy_envelope_kcal"]
# Torque is the central difference of energy around 180 degrees, then odd-extended.
pairs = []
for i, ang in enumerate(angles):
    phi = math.radians(ang - 180.0)
    prev = energy[i - 1]
    nxt = energy[(i + 1) % len(energy)]
    dphi = math.radians(angles[1] - angles[0])
    torque = -(nxt - prev) / (2.0 * dphi)
    pairs.append((phi, torque))

# Four-sine least squares, normal equations.
order = 4
ata = [[0.0 for _ in range(order)] for _ in range(order)]
atb = [0.0 for _ in range(order)]
for phi, torque in pairs:
    cols = [math.sin((k + 1) * phi) for k in range(order)]
    for r in range(order):
        atb[r] += cols[r] * torque
        for c in range(order):
            ata[r][c] += cols[r] * cols[c]

def solve(a, b):
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for i in range(n):
        pivot = max(range(i, n), key=lambda r: abs(m[r][i]))
        m[i], m[pivot] = m[pivot], m[i]
        div = m[i][i] or 1.0
        for c in range(i, n + 1):
            m[i][c] /= div
        for r in range(n):
            if r == i:
                continue
            factor = m[r][i]
            for c in range(i, n + 1):
                m[r][c] -= factor * m[i][c]
    return [m[i][n] for i in range(n)]

coeff = solve(ata, atb)
scale = max(abs(v) for v in coeff) or 1.0
print("torque coeffs", [round(v / scale, 4) for v in coeff])
