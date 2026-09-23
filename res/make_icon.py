#!/usr/bin/env python3
"""纯 Python 生成应用图标 app.ico（不依赖 Pillow）。

设计：圆角方形徽标 + 蓝色竖向渐变 + 顶部高光 + 白色对勾。
先用解析式覆盖率在 3 倍超采样下渲染 256x256 主图，再盒式降采样出其余尺寸。
"""
from __future__ import annotations

import math
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.ico")
SIZES = [256, 128, 64, 48, 32, 24, 16]
SS = 3  # 超采样倍数

# 渐变端点（上→下）
TOP = (0x5B, 0x97, 0xFF)
BOT = (0x25, 0x5F, 0xE0)


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


def render_master(n: int = 256) -> bytearray:
    """返回 n*n 的 RGBA 字节数组。

    注意：所有距离都在「归一化坐标」下计算（整图跨 -1..1，即 2 个单位），
    因此抗锯齿宽度必须乘上 P = 每单位对应的采样数，否则边缘会糊掉整张图。
    """
    m = n * SS
    P = m / 2.0  # 每个归一化单位对应多少采样点
    buf = bytearray(n * n * 4)
    acc = [0.0] * (n * n * 4)
    # 徽标几何（归一化坐标）
    hw, hh, rad = 0.455, 0.455, 0.225
    # 对勾折线（必须留在 ±0.455 的圆角矩形内，且留出描边宽度）
    pts = [(-0.260, 0.015), (-0.070, 0.205), (0.280, -0.205)]
    stroke = 0.115
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
            # 渐变
            t = (py + 1.0) * 0.5
            r = TOP[0] + (BOT[0] - TOP[0]) * t
            g = TOP[1] + (BOT[1] - TOP[1]) * t
            b = TOP[2] + (BOT[2] - TOP[2]) * t
            # 顶部高光
            hi = clamp01((0.20 - py) / 0.85) * 0.18
            r += (255 - r) * hi
            g += (255 - g) * hi
            b += (255 - b) * hi
            # 对勾
            d = min(dist_to_seg(px, py, pts[0][0], pts[0][1], pts[1][0], pts[1][1]),
                    dist_to_seg(px, py, pts[1][0], pts[1][1], pts[2][0], pts[2][1]))
            k = clamp01(0.5 + (stroke - d) * P)
            if k > 0.0:
                r += (255.0 - r) * k
                g += (255.0 - g) * k
                b += (255.0 - b) * k
            acc[idx + 0] += r * cov
            acc[idx + 1] += g * cov
            acc[idx + 2] += b * cov
            acc[idx + 3] += 255.0 * cov
    cnt = float(SS * SS)
    for i in range(n * n):
        j = i * 4
        a = acc[j + 3]
        if a > 0.0:
            # 边缘像素按覆盖率归一化，避免出现暗边
            buf[j] = min(255, int(acc[j + 0] * 255.0 / a + 0.5))
            buf[j + 1] = min(255, int(acc[j + 1] * 255.0 / a + 0.5))
            buf[j + 2] = min(255, int(acc[j + 2] * 255.0 / a + 0.5))
            buf[j + 3] = min(255, int(a / cnt + 0.5))
    return buf


def downsample(master: list, src: int, dst: int) -> bytearray:
    if dst == src:
        return bytearray(master)
    f = src // dst
    out = bytearray(dst * dst * 4)
    for y in range(dst):
        for x in range(dst):
            a = r = g = b = 0
            for dy in range(f):
                base = ((y * f + dy) * src + x * f) * 4
                for dx in range(f):
                    j = base + dx * 4
                    a += master[j + 3]
                    r += master[j + 0] * master[j + 3]
                    g += master[j + 1] * master[j + 3]
                    b += master[j + 2] * master[j + 3]
            k = (y * dst + x) * 4
            n = f * f
            aa = a // n
            out[k + 3] = aa
            if a:
                out[k + 0] = min(255, r // a)
                out[k + 1] = min(255, g // a)
                out[k + 2] = min(255, b // a)
    return out


def bmp_payload(rgba: bytearray, n: int) -> bytes:
    """ICO 里的 BMP：BITMAPINFOHEADER + 自下而上的 BGRA + AND 掩码。"""
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
