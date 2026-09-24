#!/usr/bin/env python3
"""Generate app.ico with pure Python (no Pillow).

Design: dark slate rounded-square badge + thin hexagon ring + solid inner triangle.
Rendered analytically at 3x supersampling, then box-downsampled to each icon size.
"""
from __future__ import annotations

import math
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.ico")
SIZES = [256, 128, 64, 48, 32, 24, 16]
SS = 3  # supersampling factor

# Badge gradient (top -> bottom): near-black slate
TOP = (0x1E, 0x29, 0x3B)
BOT = (0x0B, 0x11, 0x20)
# Hexagon ring: muted slate; inner triangle: deep teal
RING = (0x64, 0x74, 0x8B)
MARK = (0x0D, 0x94, 0x88)


def clamp01(v: float) -> float:
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def sd_round_rect(px: float, py: float, hw: float, hh: float, r: float) -> float:
    qx = abs(px) - (hw - r)
    qy = abs(py) - (hh - r)
    ax = qx if qx > 0.0 else 0.0
    ay = qy if qy > 0.0 else 0.0
    return math.hypot(ax, ay) + min(max(qx, qy), 0.0) - r


def dist_to_seg(px, py, ax, ay, bx, by) -> float:
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    d = vx * vx + vy * vy
    t = 0.0 if d == 0.0 else (wx * vx + wy * vy) / d
    t = clamp01(t)
    return math.hypot(px - (ax + t * vx), py - (ay + t * vy))


def polygon_pts(radius: float, n: int, rot_deg: float) -> list:
    return [(radius * math.cos(math.radians(rot_deg + 360.0 * i / n)),
             radius * math.sin(math.radians(rot_deg + 360.0 * i / n)))
            for i in range(n)]


def sd_polygon(px: float, py: float, pts: list) -> float:
    """Signed distance to a convex polygon (negative inside)."""
    inside = True
    max_edge = -1e9
    min_seg = 1e9
    m = len(pts)
    for i in range(m):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % m]
        ex, ey = bx - ax, by - ay
        # outward normal (polygon wound counter-clockwise in screen space with y up)
        nx, ny = ey, -ex
        d = ((px - ax) * nx + (py - ay) * ny) / math.hypot(nx, ny)
        if d > 0.0:
            inside = False
        if d > max_edge:
            max_edge = d
        s = dist_to_seg(px, py, ax, ay, bx, by)
        if s < min_seg:
            min_seg = s
    return max_edge if inside else min_seg


def render_master(n: int = 256) -> bytearray:
    """Return an n*n RGBA byte array.

    All distances are computed in normalized coordinates (the image spans -1..1,
    i.e. 2 units), so the antialiasing width must be multiplied by P = samples
    per unit, otherwise edges blur across the whole image.
    """
    m = n * SS
    P = m / 2.0
    buf = bytearray(n * n * 4)
    acc = [0.0] * (n * n * 4)
    # Badge geometry (normalized coordinates)
    hw, hh, rad = 0.455, 0.455, 0.225
    # Mark: pointy-top hexagon ring + inner triangle
    hex_pts = polygon_pts(0.30, 6, 90.0)
    tri_pts = polygon_pts(0.155, 3, -90.0)
    ring_w = 0.042
    for sy in range(m):
        py = (sy + 0.5) / m * 2.0 - 1.0
        row = sy // SS
        for sx in range(m):
            px = (sx + 0.5) / m * 2.0 - 1.0
            col = sx // SS
            idx = (row * n + col) * 4
            cov = clamp01(0.5 - sd_round_rect(px, py, hw, hh, rad) * P)
            if cov <= 0.0:
                continue
            # gradient
            t = (py + 1.0) * 0.5
            r = TOP[0] + (BOT[0] - TOP[0]) * t
            g = TOP[1] + (BOT[1] - TOP[1]) * t
            b = TOP[2] + (BOT[2] - TOP[2]) * t
            # hexagon ring
            d_hex = sd_polygon(px, py, hex_pts)
            k = clamp01(0.5 + (ring_w - abs(d_hex)) * P)
            if k > 0.0:
                r += (RING[0] - r) * k
                g += (RING[1] - g) * k
                b += (RING[2] - b) * k
            # inner triangle
            d_tri = sd_polygon(px, py, tri_pts)
            k = clamp01(0.5 - d_tri * P)
            if k > 0.0:
                r += (MARK[0] - r) * k
                g += (MARK[1] - g) * k
                b += (MARK[2] - b) * k
            acc[idx + 0] += r * cov
            acc[idx + 1] += g * cov
            acc[idx + 2] += b * cov
            acc[idx + 3] += 255.0 * cov
    cnt = float(SS * SS)
    for i in range(n * n):
        j = i * 4
        a = acc[j + 3]
        if a > 0.0:
            # normalize edge pixels by coverage to avoid dark fringes
            buf[j] = min(255, int(acc[j + 0] * 255.0 / a + 0.5))
            buf[j + 1] = min(255, int(acc[j + 1] * 255.0 / a + 0.5))
            buf[j + 2] = min(255, int(acc[j + 2] * 255.0 / a + 0.5))
            buf[j + 3] = min(255, int(a / cnt + 0.5))
    return buf


def downsample(master: list, src: int, dst: int) -> bytearray:
    if dst == src:
        return bytearray(master)
    # Area-weighted box resample: an integer factor (src // dst) silently drops the
    # right/bottom remainder when src is not divisible (e.g. 256 -> 48/24).
    out = bytearray(dst * dst * 4)
    scale = src / dst
    for y in range(dst):
        y0, y1 = y * scale, (y + 1) * scale
        for x in range(dst):
            x0, x1 = x * scale, (x + 1) * scale
            a = r = g = b = 0.0
            for sy in range(int(y0), min(int(y1) + 1, src)):
                wy = min(y1, sy + 1) - max(y0, sy)
                if wy <= 0:
                    continue
                for sx in range(int(x0), min(int(x1) + 1, src)):
                    wx = min(x1, sx + 1) - max(x0, sx)
                    if wx <= 0:
                        continue
                    w = wx * wy
                    j = (sy * src + sx) * 4
                    a += master[j + 3] * w
                    r += master[j + 0] * master[j + 3] * w
                    g += master[j + 1] * master[j + 3] * w
                    b += master[j + 2] * master[j + 3] * w
            k = (y * dst + x) * 4
            out[k + 3] = min(255, int(a / (scale * scale) + 0.5))
            if a:
                out[k + 0] = min(255, int(r / a + 0.5))
                out[k + 1] = min(255, int(g / a + 0.5))
                out[k + 2] = min(255, int(b / a + 0.5))
    return out


def bmp_payload(rgba: bytearray, n: int) -> bytes:
    """BMP inside an ICO: BITMAPINFOHEADER + bottom-up BGRA + AND mask."""
    hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, n * n * 4, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(n - 1, -1, -1):
        base = y * n * 4
        for x in range(n):
            j = base + x * 4
            xor += bytes((rgba[j + 2], rgba[j + 1], rgba[j + 0], rgba[j + 3]))
    row = ((n + 31) // 32) * 4
    and_mask = bytearray()
    for y in range(n - 1, -1, -1):
        line = bytearray(row)
        for x in range(n):
            if rgba[(y * n + x) * 4 + 3] < 128:
                line[x >> 3] |= 0x80 >> (x & 7)
        and_mask += line
    return hdr + bytes(xor) + bytes(and_mask)


def main() -> None:
    master = render_master(256)
    images = []
    for s in SIZES:
        images.append((s, bmp_payload(downsample(master, 256, s), s)))
    n = len(images)
    out = bytearray(struct.pack("<HHH", 0, 1, n))
    offset = 6 + 16 * n
    body = bytearray()
    for s, data in images:
        out += struct.pack("<BBBBHHII", s if s < 256 else 0, s if s < 256 else 0,
                           0, 0, 1, 32, len(data), offset + len(body))
        body += data
    with open(OUT, "wb") as f:
        f.write(bytes(out) + bytes(body))
    print("wrote", OUT, os.path.getsize(OUT), "bytes,", n, "sizes")


if __name__ == "__main__":
    main()
