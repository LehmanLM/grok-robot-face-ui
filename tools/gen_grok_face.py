#!/usr/bin/env python3
"""gen_grok_face.py — 从 grok-icon-study 的 geometry-data.js 生成 C 头文件。

用法：python3 tools/gen_grok_face.py [geometry-data.js 路径] > grok_face_data.h

数据来源：blessonism/grok-icon-study（Grok Bot.app v0.18.0 逆向学习复刻，
素材归 xAI/相应权利人所有，本项目仅个人学习使用，不商用不再分发）。
生成内容：
  - GROK_BLOB_PTS：blobPath SVG 贝塞尔展平为多边形（e=4，同 flattenPath）
  - GROK_EYES：25 组眼睛形状 × 左右 × 48 顶点（viewBox 259x259 坐标）
  - GROK_CR：relRot(POSE{17,-14,29}, POSE_HOME{33,-19,38}) 3x3 矩阵
    （登录页 eyeTopology 常量，眼睛在圆盘表面的 3D 投影用）
"""
import json
import math
import sys

GEO_PATH = sys.argv[1] if len(sys.argv) > 1 else \
    "/tmp/opencode/grok-study/replica/geometry-data.js"

raw = open(GEO_PATH).read()
start = raw.index("{")
depth = 0
end = start
for j in range(start, len(raw)):
    if raw[j] == "{":
        depth += 1
    elif raw[j] == "}":
        depth -= 1
        if depth == 0:
            end = j + 1
            break
geo = json.loads(raw[start:end])


def flatten_path(d, e=4.0):
    """SVG path (M/L/C/Q/Z) 展平为折线顶点，同 math.js flattenPath。"""
    import re
    toks = re.findall(r"[MLCQZmlcqz]|-?\d*\.?\d+(?:e[-+]?\d+)?", d)
    pts = []
    r = 0
    cmd = ""
    x0 = y0 = 0.0
    sx = sy = 0.0

    def rd():
        nonlocal r
        v = float(toks[r]); r += 1
        return v

    def seg(f, length):
        n = max(2, math.ceil(length / e))
        for k in range(1, n + 1):
            pts.append(f(k / n))

    while r < len(toks):
        if re.match(r"[a-zA-Z]", toks[r]):
            cmd = toks[r].upper(); r += 1
        if cmd == "Z":
            if math.hypot(x0 - sx, y0 - sy) > 0.01:
                seg(lambda f: (sx + (x0 - sx) * f, sy + (y0 - sy) * f),
                    math.hypot(x0 - sx, y0 - sy))
            sx, sy = x0, y0
            continue
        if r >= len(toks):
            break
        if cmd == "M":
            sx = rd(); sy = rd(); x0, y0 = sx, sy
            pts.append((x0, y0)); cmd = "L"
        elif cmd == "L":
            nx, ny = rd(), rd()
            seg(lambda f: (x0 + (nx - x0) * f, y0 + (ny - y0) * f),
                math.hypot(nx - x0, ny - y0))
            x0, y0 = nx, ny
        elif cmd == "Q":
            cx, cy, nx, ny = rd(), rd(), rd(), rd()
            px, py = x0, y0
            seg(lambda f: ((1-f)*(1-f)*px + 2*(1-f)*f*cx + f*f*nx,
                           (1-f)*(1-f)*py + 2*(1-f)*f*cy + f*f*ny),
                math.hypot(cx-px, cy-py) + math.hypot(nx-cx, ny-cy))
            x0, y0 = nx, ny
        elif cmd == "C":
            c1x, c1y, c2x, c2y, nx, ny = rd(), rd(), rd(), rd(), rd(), rd()
            px, py = x0, y0
            seg(lambda f: ((1-f)**3*px + 3*(1-f)**2*f*c1x + 3*(1-f)*f*f*c2x + f**3*nx,
                           (1-f)**3*py + 3*(1-f)**2*f*c1y + 3*(1-f)*f*f*c2y + f**3*ny),
                math.hypot(c1x-px, c1y-py) + math.hypot(c2x-c1x, c2y-c1y) + math.hypot(nx-c2x, ny-c2y))
            x0, y0 = nx, ny
        else:
            r += 1
    return pts


def rot3(turn, tilt, roll):
    d = math.pi / 180
    ct, st = math.cos(turn * d), math.sin(turn * d)
    cp, sp = math.cos(tilt * d), math.sin(tilt * d)
    cr, sr = math.cos(roll * d), math.sin(roll * d)
    return [
        cr*ct - sr*sp*st, -sr*cp, cr*st + sr*sp*ct,
        sr*ct + cr*sp*st,  cr*cp, sr*st - cr*sp*ct,
        -sp*st,            sp,    cp*ct,
    ]


def mul3(a, b):
    """a @ b^T? — character.js relRot: gn * Gn^T 逐元素即 a·b 列向量。"""
    out = [0.0] * 9
    for i in range(3):
        for j in range(3):
            out[i*3+j] = sum(a[i*3+k] * b[j*3+k] for k in range(3))
    return out


blob = flatten_path(geo["blobPath"])
eyes = geo["eyes"]
cr = mul3(rot3(17, -14, 29), rot3(33, -19, 38))  # relRot(POSE, POSE_HOME)

print("/* grok_face_data.h — 自动生成，勿手改（tools/gen_grok_face.py）")
print(" * 几何数据源：Grok Bot.app v0.18.0（经 blessonism/grok-icon-study 提取，")
print(" * 素材归 xAI/相应权利人所有；本项目个人学习用途，不商用不再分发）")
print(" * 坐标系：viewBox 259x259（minX=minY=-15），blob 中心 Re=%.4f */" % geo["Re"])
print()
print("#ifndef GROK_FACE_DATA_H")
print("#define GROK_FACE_DATA_H")
print()
print("#define GROK_RE     %.4f" % geo["Re"])
print("#define GROK_G9E    %d" % geo["G9e"])
print("#define GROK_VJT    %d" % geo["VJt"])
print("#define GROK_TOP    %.2f" % geo["shapes"]["blob"]["top"])
print("#define GROK_BOTTOM %.2f" % geo["shapes"]["blob"]["bottom"])
print()
print("static const float GROK_CR[9] = {")
print("    " + ", ".join("%.8ff" % v for v in cr))
print("};")
print()
print("/* blob 身体轮廓（展平多边形，viewBox 坐标） */")
print("#define GROK_BLOB_N %d" % len(blob))
print("static const float GROK_BLOB_PTS[GROK_BLOB_N][2] = {")
for i in range(0, len(blob), 4):
    row = blob[i:i+4]
    print("    " + " ".join("{%.2ff,%.2ff}," % p for p in row))
print("};")
print()
print("/* 眼睛形状：25 组 × [左,右] × 48 顶点 */")
print("#define GROK_EYE_N 25")
print("#define GROK_VERT_N 48")
print("static const float GROK_EYES[GROK_EYE_N][2][GROK_VERT_N][2] = {")
for shape in eyes:
    print("    {")
    for lr in shape:
        print("        {")
        for i in range(0, len(lr), 4):
            row = lr[i:i+4]
            print("            " + " ".join("{%.2ff,%.2ff}," % tuple(p) for p in row))
        print("        },")
    print("    },")
print("};")
print()
print("#endif /* GROK_FACE_DATA_H */")
