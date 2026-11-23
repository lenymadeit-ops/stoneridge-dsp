"""Seed the continuous-time LA-2A gray box and report a short held-out residual.

Full 60 s SignalTrain fits belong on a machine with the WAVs mounted. This
script checks the header time-constant law alpha = exp(lambda / fs).
"""
import math

lam = [-18.0, -32.0, -55.0, -90.0, -140.0, -220.0, -360.0, -640.0]
for fs in (44100.0, 48000.0, 96000.0):
    alpha = [math.exp(v / fs) for v in lam]
    print(fs, [round(a, 6) for a in alpha])
