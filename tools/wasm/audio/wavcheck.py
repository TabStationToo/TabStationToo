#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+
"""Check emulator audio without listening to it.

  wavcheck.py info FILE...                    format and length
  wavcheck.py check FILE [--json OUT]          levels, RMS over time, silence, clipping, gaps, dropouts,
                                               repeated frames and clicks
  wavcheck.py fingerprint FILE [--json OUT]    spectral fingerprint per second
  wavcheck.py compare REF TEST [--drift S]     alignment by cross-correlation and similarity scores
  wavcheck.py damage IN OUT [--seed N]         a copy with known gaps, dropouts, clicks and cuts (+ OUT.json)
  wavcheck.py selftest FILE                    damages a copy of FILE and reports what the detectors find

Reads PCM (16/24/32-bit) and IEEE float WAV, including the headless runner's -wav files and files whose
header was never completed. Needs numpy. Times are seconds from the start of the file; levels are dBFS
(RMS or peak of the samples, full scale 1.0).
"""

import argparse
import json
import math
import os
import random
import struct
import sys

try:
    import numpy as np
except ImportError:  # pragma: no cover
    sys.exit("wavcheck.py needs numpy (python3 -m pip install numpy)")

SILENCE_DB = -60.0  # a 10 ms frame quieter than this is silent
ACTIVE_DB = -45.0  # audio around a gap must be at least this loud for the gap to count
MIN_SILENCE_S = 0.2  # shorter quiet stretches are pauses, not silence spans
GAP_MIN_FRAMES = 24  # 0.5 ms of exact zeros on both channels inside audio
CLIP_LEVEL = 0.999
CLICK_THRESHOLD = 20.0  # matched-filter LPC residual over its robust scale in the block
CLICK_FLOOR = 2e-3  # events the size of an impulse smaller than this never count (-54 dBFS)
LPC_ORDER = 24
LPC_BLOCK = 2048
BANDS = 24
BAND_LO = 40.0
BAND_HI = 20000.0
SPARK = " .:-=+*#%@"


# ----------------------------------------------------------------------------------------------------------
# WAV files


class Wav:
    def __init__(self, path, samples, rate, fmt, bits, notes):
        self.path = path
        self.x = samples  # float32 array, frames x channels
        self.rate = rate
        self.fmt = fmt
        self.bits = bits
        self.notes = notes

    @property
    def frames(self):
        return self.x.shape[0]

    @property
    def channels(self):
        return self.x.shape[1]

    @property
    def seconds(self):
        return self.frames / self.rate

    def mono(self):
        return self.x.astype(np.float64).mean(axis=1)


def read_wav(path):
    size = os.path.getsize(path)
    notes = []
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise ValueError(f"{path}: not a RIFF/WAVE file")
        fmt = None
        data_off = data_len = None
        pos = 12
        while pos + 8 <= size:
            f.seek(pos)
            cid, clen = struct.unpack("<4sI", f.read(8))
            if cid == b"fmt ":
                body = f.read(clen)
                tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
                if tag == 0xFFFE and len(body) >= 26:
                    tag = struct.unpack("<H", body[24:26])[0]
                fmt = (tag, channels, rate, bits)
            elif cid == b"data":
                data_off = pos + 8
                data_len = clen
                if clen == 0 or data_off + clen > size:
                    # A run that stopped before completing its header, or a size field capped at 4 GB.
                    data_len = size - data_off
                    notes.append("data size taken from the file length (header not completed)")
                break
            pos += 8 + clen + (clen & 1)
        if fmt is None or data_off is None:
            raise ValueError(f"{path}: no fmt or data chunk")
        tag, channels, rate, bits = fmt
        width = bits // 8
        frames = data_len // (width * channels)
        f.seek(data_off)
        raw = np.fromfile(f, dtype=np.uint8, count=frames * width * channels)
    if tag == 3 and bits == 32:
        x = raw.view("<f4").astype(np.float32)
        kind = "float32"
    elif tag == 3 and bits == 64:
        x = raw.view("<f8").astype(np.float32)
        kind = "float64"
    elif tag == 1 and bits == 16:
        x = raw.view("<i2").astype(np.float32) / 32768.0
        kind = "pcm16"
    elif tag == 1 and bits == 24:
        b = raw.reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float32) / 8388608.0
        kind = "pcm24"
    elif tag == 1 and bits == 32:
        x = raw.view("<i4").astype(np.float32) / 2147483648.0
        kind = "pcm32"
    else:
        raise ValueError(f"{path}: unsupported format tag {tag} with {bits} bits")
    return Wav(path, x.reshape(-1, channels), rate, kind, bits, notes)


def write_wav(path, x, rate):
    """32-bit float WAV, the same layout as the headless runner's."""
    x = np.ascontiguousarray(x, dtype="<f4")
    frames, channels = x.shape
    data = frames * channels * 4
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 26 + 12 + 8 + data) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHHH", 18, 3, channels, rate, rate * channels * 4, channels * 4, 32, 0))
        f.write(b"fact" + struct.pack("<II", 4, frames))
        f.write(b"data" + struct.pack("<I", data))
        f.write(x.tobytes())


# ----------------------------------------------------------------------------------------------------------
# Helpers


def db(v, floor=-120.0):
    v = np.asarray(v, dtype=np.float64)
    with np.errstate(divide="ignore"):
        out = 20.0 * np.log10(np.maximum(v, 1e-12))
    return np.maximum(out, floor)


def frame_rms(x, n):
    """RMS of consecutive n-frame windows (the last partial window is dropped), max over channels."""
    m = (x.shape[0] // n) * n
    if m == 0:
        return np.zeros(0)
    xx = x[:m].astype(np.float64).reshape(-1, n, x.shape[1])
    return np.sqrt((xx * xx).mean(axis=1)).max(axis=1)


def runs(mask):
    """(start, end) index pairs of the True runs in a boolean array."""
    if mask.size == 0:
        return []
    d = np.diff(np.concatenate(([0], mask.astype(np.int8), [0])))
    return list(zip(np.flatnonzero(d == 1).tolist(), np.flatnonzero(d == -1).tolist()))


def spark(values_db, lo=-80.0, hi=0.0):
    out = []
    for v in values_db:
        t = (v - lo) / (hi - lo)
        out.append(SPARK[int(round(min(max(t, 0.0), 1.0) * (len(SPARK) - 1)))])
    return "".join(out)


def fmt_t(t):
    m, s = divmod(t, 60.0)
    return f"{int(m)}:{s:06.3f}" if m else f"{s:.3f}"


# ----------------------------------------------------------------------------------------------------------
# Defect detectors


def levinson(r, order):
    a = np.zeros(order + 1)
    a[0] = 1.0
    err = r[0]
    for i in range(1, order + 1):
        acc = r[i] + np.dot(a[1:i], r[i - 1:0:-1])
        k = -acc / err
        a[1:i + 1] = a[1:i + 1] + k * a[i - 1::-1][:i]
        err *= 1.0 - k * k
        if err <= 0:
            break
    return a


def lpc_click_scores(ch):
    """Per sample: a click score and the size of the impulse that would explain it.

    A 24th-order linear predictor per 2048-frame block whitens the audio; an impulse or a cut (a jump in the
    waveform) then shows as the predictor's impulse response in the residual, which the residual filtered with
    the same coefficients run backwards (a matched filter) folds back into one sample. The score is that value
    over its robust scale in the block, measured where the audio is above -80 dBFS; the size is the value over
    the coefficients' energy, i.e. the amplitude of an impulse giving the same peak."""
    n = ch.shape[0]
    x = ch.astype(np.float64)
    score = np.zeros(n)
    size = np.zeros(n)
    p = LPC_ORDER
    half = LPC_BLOCK // 2
    win = np.hanning(2 * LPC_BLOCK)
    for start in range(0, n, LPC_BLOCK):
        end = min(start + LPC_BLOCK, n)
        lo = max(0, start - p)
        seg = x[lo:min(n, end + p)]
        if seg.size <= 2 * p or not np.any(seg):
            continue
        # Coefficients from a window twice the block, centred on it: every sample of the block is modelled
        # (a window the size of the block fades its edges out of the fit, and whatever sits there looks like a
        # click or not depending on where the blocks fall).
        c0 = start - half
        ctx = x[max(0, c0):min(n, c0 + 2 * LPC_BLOCK)]
        w = ctx * win[max(0, -c0):max(0, -c0) + ctx.size]
        r = np.array([np.dot(w[: w.size - k], w[k:]) for k in range(p + 1)])
        if r[0] <= 1e-12:
            continue
        r[0] *= 1.0 + 1e-6  # tiny white-noise floor keeps the recursion stable
        a = levinson(r, p)
        e = np.convolve(seg, a, mode="full")[: seg.size]
        if start == 0:
            e[:p] = 0.0
        e2 = np.correlate(e, a, mode="full")[a.size - 1:a.size - 1 + seg.size]
        v = e2[start - lo:end - lo]
        live = np.abs(v[np.abs(x[start:end]) > 1e-4])
        if live.size < 64:
            continue
        sigma = 1.4826 * np.median(live) + 1e-12
        score[start:end] = np.abs(v) / sigma
        size[start:end] = np.abs(v) / float(np.dot(a, a))
    return score, size


def detect_clicks(x, rate, threshold=CLICK_THRESHOLD):
    """Click events (hits within 1 ms are one event) with their score, size and the level 20 ms before and after:
    "onset" and "stop" when the level changes by more than 6 dB across them (how sounds start and end in game
    audio), "mid" otherwise (where a lost chunk or a glitch in a stream lands). Also returns the times of weaker
    candidates (half the threshold), so a comparison does not call a click new only because it crossed the
    threshold in one file and not the other."""
    strength = np.zeros(x.shape[0])
    amp = np.zeros(x.shape[0])
    for c in range(x.shape[1]):
        s, z = lpc_click_scores(x[:, c])
        s = np.where(z > CLICK_FLOOR, s, 0.0)
        better = s > strength
        strength = np.where(better, s, strength)
        amp = np.where(better, z, amp)
    weak = np.flatnonzero(strength > threshold / 2.0)
    candidates = [] if weak.size == 0 else \
        (weak[np.concatenate(([0], np.flatnonzero(np.diff(weak) > rate // 1000) + 1))] / rate).round(4).tolist()
    events = []
    idx = np.flatnonzero(strength > threshold)
    ctx = rate // 50
    if idx.size:
        group = rate // 1000
        starts = np.concatenate(([0], np.flatnonzero(np.diff(idx) > group) + 1))
        ends = np.concatenate((starts[1:], [idx.size]))
        for s0, s1 in zip(starts, ends):
            seg = idx[s0:s1]
            peak = int(seg[np.argmax(strength[seg])])
            pre = float(db(np.sqrt(np.mean(x[max(0, peak - ctx):max(1, peak - 2)].astype(np.float64) ** 2))))
            post = float(db(np.sqrt(np.mean(x[peak + 2:peak + ctx].astype(np.float64) ** 2))))
            kind = "onset" if post - pre > 6.0 else ("stop" if pre - post > 6.0 else "mid")
            events.append({"t": peak / rate, "frame": peak, "strength": round(min(float(strength[peak]), 1e4), 1),
                           "size": round(float(amp[peak]), 4), "kind": kind, "before_db": round(pre, 1),
                           "after_db": round(post, 1)})
    return events, candidates


def detect_gaps(x, rate, active_db=ACTIVE_DB):
    """Exact digital zeros on every channel for at least GAP_MIN_FRAMES, with audio on both sides: what an
    underrunning stream (memset) or a lost buffer leaves. Silence that fades in or out does not count."""
    zero = np.all(x == 0.0, axis=1)
    ctx = max(1, rate // 100)  # 10 ms on each side
    out = []
    for s, e in runs(zero):
        if e - s < GAP_MIN_FRAMES or s < ctx or e + ctx > x.shape[0]:
            continue
        before = float(db(np.sqrt(np.mean(x[s - ctx:s].astype(np.float64) ** 2))))
        after = float(db(np.sqrt(np.mean(x[e:e + ctx].astype(np.float64) ** 2))))
        if before >= active_db and after >= active_db:
            out.append({"t": s / rate, "frame": s, "ms": round((e - s) * 1000.0 / rate, 2),
                        "before_db": round(before, 1), "after_db": round(after, 1)})
    return out


def detect_dropouts(x, rate, active_db=ACTIVE_DB, depth_db=30.0, max_ms=100):
    """Short stretches (2-100 ms) at least depth_db below the audio right before and after them, that are not
    exact zeros (those are gaps): a lossy path's version of a gap."""
    n1 = max(1, rate // 1000)
    env = db(frame_rms(x, n1))
    ctx = 10
    reach = max_ms + ctx
    if env.size <= 2 * reach:
        return []
    # Quieter than the loudest audio within reach by depth_db: candidate dips, then checked edge by edge.
    local = np.lib.stride_tricks.sliding_window_view(np.pad(env, reach, mode="edge"), 2 * reach + 1).max(axis=1)
    quiet = env < local - depth_db
    out = []
    for s, e in runs(quiet):
        if e - s < 2 or (e - s) > max_ms or s < ctx or e + ctx > env.size:
            continue
        before = float(np.median(env[s - ctx:s]))
        after = float(np.median(env[e:e + ctx]))
        level = float(env[s:e].max())
        if min(before, after) < active_db or min(before, after) - level < depth_db:
            continue
        seg = x[s * n1:e * n1]
        if np.all(seg == 0.0):
            continue  # reported as a gap
        out.append({"t": s * n1 / rate, "ms": float(e - s), "level_db": round(level, 1),
                    "before_db": round(before, 1), "after_db": round(after, 1)})
    return out


def detect_repeats(x, rate):
    """Frames identical to the previous one and above -100 dBFS. After SPU2's DC filter real audio practically
    never repeats a float frame (in silence the filter parks on a tiny constant, hence the floor); AudioStream's
    underrun resampler duplicates frames."""
    same = np.all(x[1:] == x[:-1], axis=1) & (np.abs(x[1:]).max(axis=1) > 1e-5)
    total = int(same.sum())
    n10 = max(1, rate // 100)
    m = (same.size // n10) * n10
    spans = []
    if m:
        frac = same[:m].reshape(-1, n10).mean(axis=1)
        for s, e in runs(frac >= 0.2):
            spans.append({"t": s * n10 / rate, "ms": (e - s) * 10.0,
                          "fraction": round(float(frac[s:e].mean()), 2)})
    return total, spans


def detect_clipping(x, rate, level=CLIP_LEVEL):
    a = np.abs(x)
    clipped = a >= level
    over = a > 1.0
    flat = 0
    for c in range(x.shape[1]):
        col = x[:, c]
        eq = (col[1:] == col[:-1]) & (np.abs(col[1:]) >= 0.99)
        flat += sum(1 for s, e in runs(eq) if e - s >= 2)
    hit = np.flatnonzero(clipped.any(axis=1))
    # Clipped samples less than 10 ms apart are one place.
    places = [] if hit.size == 0 else hit[np.concatenate(([0], np.flatnonzero(np.diff(hit) > rate // 100) + 1))]
    return {"samples": int(clipped.sum()), "over_full_scale": int(over.sum()), "flat_tops": flat,
            "places": len(places), "first_t": [round(float(i / rate), 3) for i in places[:8]]}


def silence_spans(x, rate, silence_db=SILENCE_DB, min_s=MIN_SILENCE_S):
    n10 = max(1, rate // 100)
    env = db(frame_rms(x, n10))
    out = []
    for s, e in runs(env < silence_db):
        if (e - s) * 0.01 < min_s:
            continue
        seg = x[s * n10:e * n10]
        out.append({"start": s * 0.01, "end": e * 0.01, "s": round((e - s) * 0.01, 2),
                    "digital": bool(np.all(seg == 0.0))})
    return out, env


# ----------------------------------------------------------------------------------------------------------
# Spectral fingerprint


def band_edges(rate):
    hi = min(BAND_HI, rate / 2.0)
    return np.geomspace(BAND_LO, hi, BANDS + 1)


def fingerprint(mono, rate, step_s=1.0):
    """Per step: band energies (dB) in BANDS log-spaced bands, centroid, 85% rolloff, peak frequency (20 Hz and
    up) and flatness, from the mean power spectrum of 8192-point Hann frames (5.9 Hz bins at 48 kHz, enough for
    the narrowest band)."""
    nfft = 8192
    hop = 2048
    win = np.hanning(nfft)
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    edges = band_edges(rate)
    band_of = np.digitize(freqs, edges) - 1
    valid = (band_of >= 0) & (band_of < BANDS)
    step = int(round(step_s * rate))
    rows = []
    for s in range(0, max(0, mono.size - nfft) + 1, step):
        seg = mono[s:s + max(step, nfft)]
        nfr = 1 + (seg.size - nfft) // hop if seg.size >= nfft else 0
        if nfr <= 0:
            break
        idx = np.arange(nfft)[None, :] + hop * np.arange(nfr)[:, None]
        spec = np.abs(np.fft.rfft(seg[idx] * win, axis=1)) ** 2
        p = spec.mean(axis=0) / (np.sum(win ** 2) * nfft / 2.0)
        bands = np.bincount(band_of[valid], weights=p[valid], minlength=BANDS)
        total = float(p.sum())
        rms = float(np.sqrt(np.mean(mono[s:s + step] ** 2)))
        if total > 1e-14:
            centroid = float((freqs * p).sum() / total)
            cum = np.cumsum(p)
            rolloff = float(freqs[np.searchsorted(cum, 0.85 * cum[-1])])
            lo = int(np.searchsorted(freqs, 20.0))
            peak = float(freqs[lo + np.argmax(p[lo:])])
            pp = p[1:] + 1e-20
            flat = float(np.exp(np.mean(np.log(pp))) / np.mean(pp))
        else:
            centroid = rolloff = peak = flat = 0.0
        rows.append({"t": s / rate, "rms_db": round(float(db(rms)), 1),
                     "bands_db": [round(float(v), 1) for v in db(np.sqrt(bands), -140)],
                     "centroid_hz": round(centroid), "rolloff_hz": round(rolloff), "peak_hz": round(peak),
                     "flatness": round(flat, 3)})
    return rows


def band_spark(bands_db):
    top = max(bands_db)
    if top <= -100:
        return " " * len(bands_db)
    return spark(bands_db, top - 54.0, top)  # 6 dB per step below the loudest band


def spectral_similarity(fa, fb, silent_db=-70.0):
    """Cosine similarity of band magnitudes per step: 1 for the same spectral shape at any level. Steps silent
    in both count as 1; silent in one only as 0."""
    out = []
    for ra, rb in zip(fa, fb):
        qa, qb = ra["rms_db"] < silent_db, rb["rms_db"] < silent_db
        if qa and qb:
            out.append(1.0)
            continue
        if qa != qb:
            out.append(0.0)
            continue
        va = 10 ** (np.array(ra["bands_db"]) / 20.0)
        vb = 10 ** (np.array(rb["bands_db"]) / 20.0)
        out.append(float(np.dot(va, vb) / (np.linalg.norm(va) * np.linalg.norm(vb) + 1e-30)))
    return out


# ----------------------------------------------------------------------------------------------------------
# Alignment


def xcorr_fft(a, b):
    """c[d] = sum_n a[n] * b[n + d] for d in -(len(a)-1) .. len(b)-1, returned with the lags."""
    n = a.size + b.size - 1
    nfft = 1 << (n - 1).bit_length()
    c = np.fft.irfft(np.conj(np.fft.rfft(a, nfft)) * np.fft.rfft(b, nfft), nfft)
    lags = np.concatenate((np.arange(0, b.size), np.arange(-(a.size - 1), 0)))
    vals = np.concatenate((c[: b.size], c[nfft - (a.size - 1):]))
    return lags, vals


def decimate(x, k):
    m = (x.size // k) * k
    return x[:m].reshape(-1, k).mean(axis=1)


def overlap(a, b, d):
    """The parts of a and b that line up when b[n + d] matches a[n]."""
    s = max(0, -d)
    e = min(a.size, b.size - d)
    if e <= s:
        return a[:0], b[:0]
    return a[s:e], b[s + d:e + d]


def cosine(a, b):
    den = math.sqrt(float(np.dot(a, a)) * float(np.dot(b, b)))
    return float(np.dot(a, b) / den) if den > 0 else 0.0


def align(a, b, rate, max_lag_s=None):
    """Lag d (frames) at which b[n + d] best matches a[n]: positive when b starts later. Coarse search on a
    decimated copy, then the exact lag at full rate."""
    k = max(1, rate // 6000)
    ad, bd = decimate(a, k), decimate(b, k)
    lags, vals = xcorr_fft(ad, bd)
    if max_lag_s is not None:
        keep = np.abs(lags) <= max_lag_s * rate / k
        lags, vals = lags[keep], vals[keep]
    coarse = int(lags[np.argmax(vals)]) * k
    best, best_v = coarse, -np.inf
    for d in range(coarse - 2 * k, coarse + 2 * k + 1):
        oa, ob = overlap(a, b, d)
        if oa.size == 0:
            continue
        v = float(np.dot(oa, ob))
        if v > best_v:
            best, best_v = d, v
    return best


def new_in_test(ra, ta, lag_s, drift_track, drift_s, tol=0.005):
    """The test's clicks, gaps, dropouts and repeated-frame spans with nothing of the same kind within tol in the
    reference, once test times are mapped back with the global lag (or the nearest drift window's lag)."""
    def to_ref(t):
        if drift_track:
            best = None
            for d in drift_track:
                if d["lag_ms"] is None:
                    continue
                mid = d["t"] + d["lag_ms"] / 1000.0 + drift_s / 2.0
                if best is None or abs(t - mid) < best[0]:
                    best = (abs(t - mid), d["lag_ms"] / 1000.0)
            if best is not None:
                return t - best[1]
        return t - lag_s

    out = {}
    for key, span in (("clicks", None), ("gaps", "ms"), ("dropouts", "ms"), ("repeat_spans", "ms")):
        if key == "clicks":
            ref_t = [(t, t) for t in ra["click_candidates"]]
        else:
            ref_t = [(e["t"], e["t"] + (e[span] / 1000.0 if span else 0.0)) for e in ra[key]]
        new = []
        for e in ta[key]:
            t0 = to_ref(e["t"])
            t1 = t0 + (e[span] / 1000.0 if span else 0.0)
            if not any(a - tol <= t1 and t0 <= b + tol for a, b in ref_t):
                new.append(dict(e, ref_t=round(t0, 4)))
        out[key] = new
    return out


def lag_steps(track, min_ms=0.5, min_similarity=0.5):
    """Where the drift track's lag changes between neighbouring windows: audio inserted (+) or lost (-)."""
    good = [d for d in track if d["lag_ms"] is not None and d["similarity"] >= min_similarity]
    out = []
    for a, b in zip(good, good[1:]):
        step = b["lag_ms"] - a["lag_ms"]
        if abs(step) >= min_ms:
            out.append({"from_t": a["t"], "to_t": b["t"], "ms": round(step, 3),
                        "frames": int(b["lag_frames"] - a["lag_frames"])})
    return out


def compare(ref, test, max_lag_s=None, window_s=1.0, drift_s=None, new_defects=True, analyses=None):
    if ref.rate != test.rate:
        raise ValueError(f"sample rates differ ({ref.rate} vs {test.rate})")
    rate = ref.rate
    a, b = ref.mono(), test.mono()
    lag = align(a, b, rate, max_lag_s)
    oa, ob = overlap(a, b, lag)
    res = {"ref": ref.path, "test": test.path, "rate": rate, "lag_frames": lag,
           "lag_ms": round(lag * 1000.0 / rate, 3), "overlap_s": round(oa.size / rate, 3)}
    if oa.size == 0:
        res["verdict"] = "no overlap"
        return res
    sim = cosine(oa, ob)
    gain = float(np.dot(oa, ob) / max(np.dot(ob, ob), 1e-30))
    diff = oa - gain * ob
    err = float(np.dot(diff, diff))
    sig = float(np.dot(oa, oa))
    s0 = max(0, -lag)
    ra = ref.x[s0:s0 + oa.size]
    tb = test.x[s0 + lag:s0 + lag + oa.size]
    identical = bool(lag == 0 and ref.frames == test.frames and np.array_equal(ref.x, test.x))
    res.update({"identical": identical, "overlap_identical": bool(np.array_equal(ra, tb)),
                "max_abs_diff": float(np.abs(ra.astype(np.float64) - tb).max()),
                "similarity": round(sim, 6), "gain_db": round(float(db(abs(gain), -200)), 3),
                "snr_db": (None if err == 0 else round(10 * math.log10(max(sig, 1e-30) / err), 2)),
                "channel_similarity": [round(cosine(ra[:, c].astype(np.float64), tb[:, c].astype(np.float64)), 6)
                                       for c in range(min(ref.channels, test.channels))]})
    # Per window after the global alignment.
    w = int(window_s * rate)
    wins = []
    for s in range(0, oa.size - w + 1, w):
        pa, pb = oa[s:s + w], ob[s:s + w]
        la, lb = float(db(np.sqrt(np.mean(pa * pa)))), float(db(np.sqrt(np.mean(pb * pb))))
        if la < SILENCE_DB and lb < SILENCE_DB:
            wins.append({"t": (s + s0) / rate, "similarity": None})
        else:
            wins.append({"t": (s + s0) / rate, "similarity": round(cosine(pa, pb), 4),
                         "ref_db": round(la, 1), "test_db": round(lb, 1)})
    res["windows"] = wins
    fa = fingerprint(oa, rate, window_s)
    fb = fingerprint(ob, rate, window_s)
    ss = spectral_similarity(fa, fb)
    active = [v for v, r1, r2 in zip(ss, fa, fb) if r1["rms_db"] >= -70 or r2["rms_db"] >= -70]
    res["spectral_similarity"] = round(float(np.mean(active)), 4) if active else 1.0
    res["spectral_windows"] = [round(v, 4) for v in ss]
    if drift_s:
        res["drift"] = drift(a, b, rate, lag, drift_s)
    if new_defects:
        ra, ta = analyses if analyses else (analyse(ref), analyse(test))
        res["new_in_test"] = new_in_test(ra, ta, lag / rate, res.get("drift"), drift_s)
    if identical:
        res["verdict"] = "identical"
    elif res["overlap_identical"]:
        res["verdict"] = "identical where they overlap"
    elif sim >= 0.99:
        res["verdict"] = "equivalent waveform"
    elif res["spectral_similarity"] >= 0.9:
        res["verdict"] = "similar sound, different waveform (time-stretched, resampled or shifted in places)"
    else:
        res["verdict"] = "different"
    return res


def drift(a, b, rate, lag, window_s, search_s=1.0):
    """Best lag per window of a, searched within search_s of the last window that matched well (the global lag
    at first): a lag that changes over time shows inserted or lost audio, or time-stretching."""
    w = int(window_s * rate)
    r = int(search_s * rate)
    out = []
    for s in range(0, a.size - w + 1, w):
        pa = a[s:s + w]
        if float(db(np.sqrt(np.mean(pa * pa)))) < SILENCE_DB:
            out.append({"t": s / rate, "lag_frames": None, "lag_ms": None, "similarity": None})
            continue
        lo = max(0, s + lag - r)
        hi = min(b.size, s + lag + w + r)
        seg = b[lo:hi]
        if seg.size < w:
            break
        lags, vals = xcorr_fft(pa, seg)
        keep = (lags >= 0) & (lags <= seg.size - w)
        lags, vals = lags[keep], vals[keep]
        if lags.size == 0:
            break
        d = int(lags[np.argmax(vals)])
        sim = cosine(pa, seg[d:d + w])
        out.append({"t": s / rate, "lag_frames": lo + d - s, "lag_ms": round((lo + d - s) * 1000.0 / rate, 2),
                    "similarity": round(sim, 4)})
        if sim >= 0.8:
            lag = lo + d - s
    return out


# ----------------------------------------------------------------------------------------------------------
# Commands


def analyse(w, threshold=CLICK_THRESHOLD):
    x = w.x
    rate = w.rate
    peak = np.abs(x).max(axis=0) if w.frames else np.zeros(w.channels)
    rms = np.sqrt((x.astype(np.float64) ** 2).mean(axis=0)) if w.frames else np.zeros(w.channels)
    silence, env10 = silence_spans(x, rate)
    silent_s = sum(s["s"] for s in silence)
    lin10 = frame_rms(x, max(1, rate // 100))
    active = env10 >= SILENCE_DB
    active_rms = float(np.sqrt(np.mean(lin10[active] ** 2))) if active.any() else 0.0
    per_second = db(frame_rms(x, rate)).round(1).tolist() if w.frames >= rate else []
    repeats, repeat_spans = detect_repeats(x, rate)
    clicks, candidates = detect_clicks(x, rate, threshold)
    return {
        "file": w.path, "rate": rate, "channels": w.channels, "format": w.fmt, "frames": w.frames,
        "seconds": round(w.seconds, 3), "notes": w.notes,
        "peak_db": [round(float(v), 2) for v in db(peak)],
        "rms_db": [round(float(v), 2) for v in db(rms)],
        "active_rms_db": round(float(db(active_rms)), 2),
        "active_s": round(w.seconds - silent_s, 2),
        "rms_db_per_second": per_second,
        "silence": silence,
        "clipping": detect_clipping(x, rate),
        "gaps": detect_gaps(x, rate),
        "dropouts": detect_dropouts(x, rate),
        "repeated_frames": repeats,
        "repeat_spans": repeat_spans,
        "clicks": clicks,
        "click_candidates": candidates,
    }


def print_list(title, items, fmt, limit=12):
    print(f"  {title}: {len(items)}")
    for it in items[:limit]:
        print("    " + fmt(it))
    if len(items) > limit:
        print(f"    ... {len(items) - limit} more")


def fmt_click(k):
    return (f"{fmt_t(k['t'])}  {k['kind']:5s} strength {k['strength']:7.1f}, size {k['size']:.4f} "
            f"({k['before_db']} -> {k['after_db']} dBFS)")


def print_check(r):
    print(f"{r['file']}")
    print(f"  {r['format']}, {r['channels']} ch, {r['rate']} Hz, {r['frames']} frames, {fmt_t(r['seconds'])} s")
    for n in r["notes"]:
        print(f"  note: {n}")
    print(f"  peak {' / '.join(f'{v:.1f}' for v in r['peak_db'])} dBFS, "
          f"RMS {' / '.join(f'{v:.1f}' for v in r['rms_db'])} dBFS (L / R), "
          f"active {r['active_s']:.2f} s at {r['active_rms_db']:.1f} dBFS")
    ps = r["rms_db_per_second"]
    if ps:
        print("  RMS per second (-80..0 dBFS):")
        for i in range(0, len(ps), 60):
            print(f"    {i:5d}s |{spark(ps[i:i + 60])}|")
    print_list("silence spans (< -60 dBFS for 0.2 s+)", r["silence"],
               lambda s: f"{fmt_t(s['start'])} - {fmt_t(s['end'])} ({s['s']:.2f} s{', digital zero' if s['digital'] else ''})")
    c = r["clipping"]
    print(f"  clipping: {c['samples']} samples >= {CLIP_LEVEL}, {c['over_full_scale']} over full scale, "
          f"{c['flat_tops']} flat tops" + (f", in {c['places']} places from {c['first_t']}" if c["first_t"] else ""))
    print_list("gaps (exact zeros inside audio)", r["gaps"],
               lambda g: f"{fmt_t(g['t'])}  {g['ms']:.2f} ms  ({g['before_db']} / {g['after_db']} dBFS around)")
    print_list("dropouts (30 dB dips, not zeros)", r["dropouts"],
               lambda g: f"{fmt_t(g['t'])}  {g['ms']:.0f} ms at {g['level_db']} dBFS ({g['before_db']} / {g['after_db']} around)")
    print(f"  repeated frames: {r['repeated_frames']}")
    print_list("spans with 20%+ repeated frames", r["repeat_spans"],
               lambda s: f"{fmt_t(s['t'])}  {s['ms']:.0f} ms, {s['fraction'] * 100:.0f}% repeated")
    kinds = {k: sum(1 for c in r["clicks"] if c["kind"] == k) for k in ("mid", "onset", "stop")}
    print_list(f"clicks ({kinds['mid']} mid-audio, {kinds['onset']} at onsets, {kinds['stop']} at stops)", r["clicks"],
               fmt_click)


def cmd_info(args):
    for p in args.files:
        w = read_wav(p)
        print(f"{p}: {w.fmt}, {w.channels} ch, {w.rate} Hz, {w.frames} frames, {fmt_t(w.seconds)} s"
              + "".join(f"; {n}" for n in w.notes))


def cmd_check(args):
    w = read_wav(args.file)
    r = analyse(w, args.click_threshold)
    print_check(r)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(r, f, indent=1)
    if args.strict:
        bad = len(r["gaps"]) + len(r["dropouts"]) + len(r["clicks"]) + r["clipping"]["samples"]
        return 1 if bad else 0
    return 0


def cmd_fingerprint(args):
    w = read_wav(args.file)
    rows = fingerprint(w.mono(), w.rate, args.step)
    edges = band_edges(w.rate)
    print(f"{w.path}: {len(rows)} steps of {args.step} s; bands {BAND_LO:.0f} Hz - {edges[-1]:.0f} Hz, "
          f"{BANDS} log-spaced, 6 dB per step below each row's loudest band")
    print(f"  {'t':>8} {'RMS':>6} {'centroid':>8} {'rolloff':>7} {'peak':>6} {'flat':>5}  bands")
    for r in rows:
        print(f"  {fmt_t(r['t']):>8} {r['rms_db']:6.1f} {r['centroid_hz']:8d} {r['rolloff_hz']:7d} "
              f"{r['peak_hz']:6d} {r['flatness']:5.2f}  |{band_spark(r['bands_db'])}|")
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"file": w.path, "rate": w.rate, "step_s": args.step,
                       "band_edges_hz": [round(float(e), 1) for e in edges], "steps": rows}, f, indent=1)
    return 0


def cmd_compare(args):
    ref, test = read_wav(args.ref), read_wav(args.test)
    r = compare(ref, test, args.max_lag, args.window, args.drift)
    print(f"ref  {ref.path}: {fmt_t(ref.seconds)} s")
    print(f"test {test.path}: {fmt_t(test.seconds)} s")
    print(f"  lag {r['lag_frames']} frames ({r['lag_ms']} ms; positive: the test starts later), "
          f"overlap {fmt_t(r['overlap_s'])} s")
    if "similarity" in r:
        snr = "inf" if r["snr_db"] is None else f"{r['snr_db']:.1f}"
        print(f"  waveform similarity {r['similarity']:.6f} (L {r['channel_similarity'][0]:.4f}, "
              f"R {r['channel_similarity'][-1]:.4f}), gain {r['gain_db']:+.2f} dB, SNR {snr} dB, "
              f"max |diff| {r['max_abs_diff']:.3g}")
        print(f"  spectral similarity {r['spectral_similarity']:.4f}")
        wins = [x for x in r["windows"] if x["similarity"] is not None]
        if wins:
            vals = [x["similarity"] for x in wins]
            low = [x for x in wins if x["similarity"] < args.low]
            print(f"  {args.window:g} s windows: median {np.median(vals):.4f}, min {min(vals):.4f}, "
                  f"{len(low)} below {args.low}")
            for x in low[:12]:
                print(f"    {fmt_t(x['t'])}  {x['similarity']:.4f}  ({x['ref_db']} / {x['test_db']} dBFS)")
            if len(low) > 12:
                print(f"    ... {len(low) - 12} more")
        if "drift" in r:
            print(f"  lag per {args.drift:g} s of the reference (ms, similarity):")
            line = []
            for x in r["drift"]:
                line.append(f"{fmt_t(x['t'])}: " + ("silent" if x["lag_ms"] is None else
                                                     f"{x['lag_ms']:+.1f} ({x['similarity']:.2f})"))
            for i in range(0, len(line), 4):
                print("    " + "   ".join(line[i:i + 4]))
            steps = lag_steps(r["drift"])
            print(f"  timing jumps (lag changes of 0.5 ms or more between windows): {len(steps)}")
            for st in steps[:12]:
                print(f"    between {fmt_t(st['from_t'])} and {fmt_t(st['to_t'])}: {st['ms']:+.2f} ms "
                      f"({st['frames']:+d} frames)")
    if "new_in_test" in r:
        n = r["new_in_test"]
        print(f"  in the test but not the reference: {len(n['clicks'])} clicks, {len(n['gaps'])} gaps, "
              f"{len(n['dropouts'])} dropouts, {len(n['repeat_spans'])} repeated-frame spans")
        for k in n["clicks"][:12]:
            print("    click " + fmt_click(k))
        for g in n["gaps"][:12]:
            print(f"    gap   {fmt_t(g['t'])}  {g['ms']:.2f} ms")
        for g in n["dropouts"][:6]:
            print(f"    dropout {fmt_t(g['t'])}  {g['ms']:.0f} ms")
        for g in n["repeat_spans"][:6]:
            print(f"    repeats {fmt_t(g['t'])}  {g['ms']:.0f} ms, {g['fraction'] * 100:.0f}%")
    print(f"  verdict: {r['verdict']}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(r, f, indent=1)
    if args.min_similarity is not None:
        return 0 if r.get("similarity", 0.0) >= args.min_similarity else 1
    return 0


def damage(w, seed, inserts, dropouts, clicks, cuts):
    """Returns the damaged samples and the list of defects, with their times in the damaged file."""
    rng = random.Random(seed)
    x = w.x
    rate = w.rate
    n10 = rate // 100
    env = db(frame_rms(x, n10))
    # Places with audio around them (-40 dBFS over +-30 ms), away from the ends and from each other.
    ok = env >= -40.0
    good = np.flatnonzero(np.convolve(ok, np.ones(7), "same") >= 7)
    good = good[(good > 10) & (good < env.size - 10)]
    total = inserts + dropouts + clicks + cuts
    if good.size < total:
        raise ValueError("not enough audio to damage")
    picks = []
    tries = 0
    while len(picks) < total and tries < 100000:
        tries += 1
        f = int(good[rng.randrange(good.size)])
        if all(abs(f - p) >= 20 for p in picks):  # 200 ms apart
            picks.append(f)
    if len(picks) < total:
        raise ValueError("could not place the defects 200 ms apart")
    kinds = ["insert"] * inserts + ["dropout"] * dropouts + ["click"] * clicks + ["cut"] * cuts
    rng.shuffle(kinds)
    ops = sorted(((p * n10 + rng.randrange(n10), k) for p, k in zip(picks, kinds)))
    pieces = []
    events = []
    pos = 0
    out_len = 0
    for at, kind in ops:
        pieces.append(x[pos:at])
        out_len += at - pos
        pos = at
        ev = {"kind": kind, "t": out_len / rate, "source_t": at / rate}
        if kind == "insert":
            n = int(rate * rng.uniform(0.01, 0.1))
            pieces.append(np.zeros((n, x.shape[1]), np.float32))
            out_len += n
            ev["ms"] = round(n * 1000.0 / rate, 2)
        elif kind == "dropout":
            n = int(rate * rng.uniform(0.002, 0.05))
            pieces.append(np.zeros((n, x.shape[1]), np.float32))
            out_len += n
            pos += n
            ev["ms"] = round(n * 1000.0 / rate, 2)
        elif kind == "click":
            amp = rng.uniform(0.05, 0.3) * rng.choice((-1.0, 1.0))
            c = x[pos:pos + 1].copy() + np.float32(amp)
            pieces.append(c)
            out_len += 1
            pos += 1
            ev["amplitude"] = round(amp, 3)
        else:  # a lost chunk: one or two of AudioStream's 64-frame chunks, or more
            n = rng.choice((64, 128, 256, 512))
            pos += n
            ev["frames"] = n
        events.append(ev)
    pieces.append(x[pos:])
    return np.concatenate(pieces), events


def cmd_damage(args):
    w = read_wav(args.input)
    y, events = damage(w, args.seed, args.inserts, args.dropouts, args.clicks, args.cuts)
    write_wav(args.output, y, w.rate)
    with open(args.output + ".json", "w") as f:
        json.dump({"source": w.path, "seed": args.seed, "events": events}, f, indent=1)
    for e in events:
        extra = {k: v for k, v in e.items() if k not in ("kind", "t", "source_t")}
        print(f"  {fmt_t(e['t'])}  {e['kind']:8s} {extra}")
    print(f"wrote {args.output} ({len(events)} defects) and {args.output}.json")
    return 0


def match_events(events, found, rate):
    """Which injected defects the detectors found (2 ms tolerance; a gap or dropout may be found anywhere
    inside its span). Returns found flags per event and the detections no event explains."""
    hit = [False] * len(events)
    used = [False] * len(found)
    for i, e in enumerate(events):
        lo = e["t"] - 0.002
        hi = e["t"] + (e.get("ms", 0.0) / 1000.0) + 0.002
        for j, (kind, t, span) in enumerate(found):
            want = {"insert": ("gap", "dropout"), "dropout": ("gap", "dropout"),
                    "click": ("click",), "cut": ("click",)}[e["kind"]]
            if kind in want and t <= hi and t + span >= lo:
                hit[i] = True
                used[j] = True
            elif kind == "click" and e["kind"] in ("insert", "dropout") and lo - 0.002 <= t <= hi + 0.002:
                used[j] = True  # the edges of a gap are discontinuities too
    return hit, [f for f, u in zip(found, used) if not u]


def detections(r):
    out = [("gap", g["t"], g["ms"] / 1000.0) for g in r["gaps"]]
    out += [("dropout", d["t"], d["ms"] / 1000.0) for d in r["dropouts"]]
    out += [("click", c["t"], 0.0) for c in r["clicks"]]
    return out


def cmd_selftest(args):
    w = read_wav(args.file)
    print(f"{w.path}: {fmt_t(w.seconds)} s")
    clean = analyse(w, args.click_threshold)
    base = detections(clean) + [("click", t, 0.0) for t in clean["click_candidates"]]
    print(f"  clean file: {len(clean['gaps'])} gaps, {len(clean['dropouts'])} dropouts, "
          f"{len(clean['clicks'])} clicks, {clean['repeated_frames']} repeated frames")
    y, events = damage(w, args.seed, args.inserts, args.dropouts, args.clicks, args.cuts)
    out = os.path.join(args.out_dir, os.path.splitext(os.path.basename(w.path))[0] + f"-damaged-{args.seed}.wav")
    write_wav(out, y, w.rate)
    with open(out + ".json", "w") as f:
        json.dump({"source": w.path, "seed": args.seed, "events": events}, f, indent=1)
    dw = Wav(out, y, w.rate, "float32", 32, [])
    dmg = analyse(dw, args.click_threshold)
    hit, extra = match_events(events, detections(dmg), w.rate)

    # Detections the clean file already has are not false positives: map them through the inserted and
    # removed time.
    def to_damaged(t):
        shift = 0.0
        for e in events:
            if e["source_t"] <= t:
                if e["kind"] == "insert":
                    shift += e["ms"] / 1000.0
                elif e["kind"] == "cut":
                    shift -= e["frames"] / w.rate
        return t + shift

    base_t = [to_damaged(t) for _, t, _ in base]
    false_pos = [f for f in extra if not any(abs(f[1] - t) <= 0.003 for t in base_t)]
    ok = True
    print(f"  damaged copy: {out}")
    for kind in ("insert", "dropout", "click", "cut"):
        idx = [i for i, e in enumerate(events) if e["kind"] == kind]
        if not idx:
            continue
        n = sum(hit[i] for i in idx)
        need = {"insert": 1.0, "dropout": 1.0, "click": 0.9, "cut": 0.0}[kind]
        state = "ok" if n >= need * len(idx) else "FAIL"
        ok &= state == "ok"
        missed = [fmt_t(events[i]["t"]) for i in idx if not hit[i]]
        print(f"    {kind:8s} found {n}/{len(idx)}  {state}" + (f"  missed at {', '.join(missed)}" if missed else ""))
    per_min = len(false_pos) / max(w.seconds / 60.0, 1e-9)
    fp_ok = per_min <= 1.0
    ok &= fp_ok
    print(f"    false detections {len(false_pos)} ({per_min:.2f} per minute)  {'ok' if fp_ok else 'FAIL'}")
    for f in false_pos[:8]:
        print(f"      {f[0]} at {fmt_t(f[1])}")
    cmp = compare(w, dw, None, 1.0, 0.5, analyses=(clean, dmg))
    new = cmp["new_in_test"]
    hit2, extra2 = match_events(events, detections(new), w.rate)
    low = [x for x in cmp["windows"] if x["similarity"] is not None and x["similarity"] < 0.99]
    lags = [x["lag_ms"] for x in cmp["drift"] if x["lag_ms"] is not None and x["similarity"] >= 0.8]
    shift = sum(e["ms"] for e in events if e["kind"] == "insert") - \
        sum(e["frames"] * 1000.0 / w.rate for e in events if e["kind"] == "cut")
    print(f"    compare: lag {cmp['lag_ms']} ms, similarity {cmp['similarity']:.4f}, "
          f"spectral {cmp['spectral_similarity']:.4f}, {len(low)} windows below 0.99, "
          f"drift {lags[0] if lags else 0:+.1f} -> {lags[-1] if lags else 0:+.1f} ms "
          f"(injected net shift {shift:+.1f} ms), verdict: {cmp['verdict']}")
    by_kind = ", ".join("%s %d/%d" % (k, sum(h for h, e in zip(hit2, events) if e["kind"] == k),
                                     sum(1 for e in events if e["kind"] == k)) for k in ("insert", "dropout", "click", "cut"))
    print(f"    new in the damaged copy (compare): {sum(hit2)}/{len(events)} defects found ({by_kind}), "
          f"{len(extra2)} detections no defect explains")
    # Inserted and lost audio as timing jumps of the 0.5 s drift track (reference time).
    steps = lag_steps(cmp["drift"])
    timed = [e for e in events if e["kind"] in ("insert", "cut")]
    def frames_of(e):
        return int(round(e["ms"] * w.rate / 1000.0)) if e["kind"] == "insert" else -e["frames"]

    # Defects closer together than two windows add up into one jump; check the isolated ones one by one and the
    # whole track's change against the net shift.
    isolated = [e for e in timed if all(f is e or abs(f["source_t"] - e["source_t"]) > 1.0 for f in timed)]
    exact = sum(1 for e in isolated if any(st["from_t"] - 0.01 <= e["source_t"] < st["to_t"] + 0.5 and
                                           abs(st["frames"] - frames_of(e)) <= 1 for st in steps))
    good = [x for x in cmp["drift"] if x["lag_frames"] is not None and x["similarity"] >= 0.8]
    net = (good[-1]["lag_frames"] - good[0]["lag_frames"]) if good else 0
    want = sum(frames_of(e) for e in timed if (not good) or good[0]["t"] <= e["source_t"] < good[-1]["t"])
    print(f"    timing jumps (compare --drift 0.5): {exact}/{len(isolated)} isolated inserts and cuts found "
          f"frame-exact; the lag moves {net:+d} frames over the file for {want:+d} injected")
    print("  PASS" if ok else "  FAIL")
    return 0 if ok else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("info", help="format and length")
    s.add_argument("files", nargs="+")
    s.set_defaults(fn=cmd_info)
    s = sub.add_parser("check", help="levels and defects")
    s.add_argument("file")
    s.add_argument("--json")
    s.add_argument("--click-threshold", type=float, default=CLICK_THRESHOLD)
    s.add_argument("--strict", action="store_true", help="exit 1 on any gap, dropout, click or clipped sample")
    s.set_defaults(fn=cmd_check)
    s = sub.add_parser("fingerprint", help="spectral fingerprint per second")
    s.add_argument("file")
    s.add_argument("--step", type=float, default=1.0)
    s.add_argument("--json")
    s.set_defaults(fn=cmd_fingerprint)
    s = sub.add_parser("compare", help="align two recordings and score their similarity")
    s.add_argument("ref")
    s.add_argument("test")
    s.add_argument("--max-lag", type=float, help="seconds (default: any)")
    s.add_argument("--window", type=float, default=1.0, help="seconds per similarity window")
    s.add_argument("--low", type=float, default=0.99, help="list windows below this similarity")
    s.add_argument("--drift", type=float, help="also find the lag per window of this many seconds")
    s.add_argument("--min-similarity", type=float, help="exit 1 below this waveform similarity")
    s.add_argument("--json")
    s.set_defaults(fn=cmd_compare)
    for name, fn in (("damage", cmd_damage), ("selftest", cmd_selftest)):
        s = sub.add_parser(name)
        if name == "damage":
            s.add_argument("input")
            s.add_argument("output")
        else:
            s.add_argument("file")
            s.add_argument("--out-dir", default=".", help="where the damaged copy goes")
            s.add_argument("--click-threshold", type=float, default=CLICK_THRESHOLD)
        s.add_argument("--seed", type=int, default=1)
        s.add_argument("--inserts", type=int, default=8, help="inserted silence, 10-100 ms (shifts the rest)")
        s.add_argument("--dropouts", type=int, default=8, help="audio replaced by zeros, 2-50 ms")
        s.add_argument("--clicks", type=int, default=8, help="single-sample spikes of 0.05-0.3")
        s.add_argument("--cuts", type=int, default=8, help="lost 64-512 frame chunks")
        s.set_defaults(fn=fn)
    args = p.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
