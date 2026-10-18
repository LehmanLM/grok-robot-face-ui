# grok-robot-face-ui

Grok Bot 桌面宠物表情的 C 移植，运行于 **RK3566 / Linux fbdev**，LVGL 9 只作为画布宿主。

一个 C 文件承载的完整表情移植：弹簧姿态机、3D 眼形投影、软件光栅（多边形挖洞 + 2× 超采样 AA + 渐变墨水，数学与参数按原版复刻）。表情与渲染管线均用纯 C 实现，不依赖浏览器/Canvas。

**English short**: A single-file C port of the Grok Bot desktop-pet face — spring pose engine, 3D eye projection and a software rasterizer (polygon holes, 2× supersampling, gradient ink, parameters replicated from the original). LVGL only carries the canvas; the face pipeline is plain C, no browser/Canvas involved.

## 效果速览

打开 [`docs/face-engine.html`](docs/face-engine.html)（浏览器本地打开即可）——一个网页版迷你复刻：会呼吸、眨眼、视线跟随鼠标，可拖动滑杆实时调弹簧参数，还有完整的原理讲解（流水线 / 参数表 / 状态映射）。

板上实际效果（状态驱动）：

| 状态 | 表情行为 |
|---|---|
| 待机 idle | 16 眼形轮播（3~5s 随机换形）+ 呼吸微动 + 随机动作节拍（转圈/粒子） |
| 唤醒 wake | 弹起 + 整圈旋转（spinTurn 弹簧） |
| 聆听 listen | 呼吸环（半径由 `GROK_RE × VIEW_K` 派生） |
| 思考 think | 静止 + 眼形上扬 |
| 播报 speak | 笑眼 + 弹跳 + 粒子爆发 |

画布上方实时显示当前眼形名（平静/好奇/寻找/专注/开心/怀疑…）。

## 表情引擎一帧（33ms）

```
姿态弹簧层 (16 弹簧积分, vendor 坐标系)
    └─ gf_view_to_ss(): (v−114.27)×VIEW_K+200, ×2 超采样
        └─ 光栅层: 扫描线填充 blob(set=1) + 眼睛挖洞(set=0)
           → coverage 降采样 AA → 橙渐变墨水混底色 → expr_buf[ARGB]
            └─ lv_canvas → LVGL flush 旋转 blit → /dev/fb0
```

关键宏：`EXPR_W/EXPR_H`（画布尺寸）、`VIEW_K=(EXPR_W−28)/230`（脸占满画布、中心钉画布中心）——改一个宏整条链自动缩放。

## 仓库内容

```
robot-ui.c            表情引擎 + 承载 UI（单文件）
lv_font_cn20.c        Noto Sans CJK 20px 位图字体（OFL）
tools/                几何数据生成 / 待机音效合成脚本
docs/face-engine.html 网页版原理讲解 + 可玩 demo
```

## 几何数据（需自行生成，不入仓）

版权原因，本仓库**不包含**从 Grok Bot.app 提取的几何数据（归
xAI / 相应权利人所有，上游研究仓明确请勿再分发）：

```bash
# 1. 按 grok-icon-study 仓库说明自行取得 geometry 数据文件（个人学习用途）
# 2. 生成编译所需的头文件：
python3 tools/gen_grok_face.py <geometry-data.js> > grok_face_data.h
```

## 构建（aarch64 交叉编译示例）

```bash
# 依赖：LVGL 9 头文件 + 静态库（MIT）
aarch64-linux-gnu-gcc robot-ui.c lv_font_cn20.c -o robot-ui \
    -I<lvgl>/src -I<lvgl> -llvgl -lm -lpthread
# 运行环境变量：ROBOT_UI_ROT=90|270（fbdev 旋转）、HRES/VRES（逻辑分辨率）
```

## 授权与声明

- **本项目代码（移植实现部分）**：MIT（见 `LICENSE`）
- **表情几何数据**：提取自 xAI **Grok Bot.app** v0.18.0，经
  [blessonism/grok-icon-study](https://github.com/blessonism/grok-icon-study)
  整理，**版权归 xAI / 相应权利人所有，且上游明确请勿再分发——因此
  本仓库不包含任何几何数据文件**，请使用者按上游说明自行获取并生成
  （仅个人学习用途，勿商用）。感谢原版设计。
- **字体** `lv_font_cn20.c`：由 Noto Sans CJK SC 生成，SIL OFL 1.1
- **LVGL**：MIT，[lvgl.io](https://lvgl.io)
- 详细第三方清单见 `THIRD_PARTY_NOTICES.md`
