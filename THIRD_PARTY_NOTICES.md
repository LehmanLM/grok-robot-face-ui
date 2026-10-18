# 第三方素材与数据声明 / Third-Party Notices

## 1. 表情几何数据（xAI Grok Bot）——**不包含在本仓库中**

- 相关文件：`vendor-geometry-data.js`、`grok_face_data.h`（本地自行生成，均不入仓）
- 来源：xAI **Grok Bot.app** v0.18.0 桌面宠物，经公开研究仓
  [blessonism/grok-icon-study](https://github.com/blessonism/grok-icon-study)
  提取整理（`app.asar` 内 `index-UbX-y3il.js`）。
- **版权归 xAI / 相应权利人所有，且上游研究仓明确"请勿再分发"——
  因此本仓库不分发任何几何数据**，仅提供生成脚本；
  使用者请按上游说明自行获取数据并仅作个人学习用途，勿商用。
- 自有实现部分（弹簧参数表按原版数值复刻手感）同样视为对原版设计的
  学习性复现。

## 2. 中文字体（SIL OFL 1.1）

- 文件：`lv_font_cn20.c`（LVGL 位图字体，GB2312 常用字 20px）
- 来源：由 **Noto Sans CJK SC**（Regular）生成，
  遵循 [SIL Open Font License 1.1](https://openfontlicense.org/)，
  允许自由使用、研究、嵌入与再分发。

## 3. LVGL（MIT）

- 本项目 UI 层基于 [LVGL 9](https://github.com/lvgl/lvgl)（MIT License），
  LVGL 源码不包含在本仓库中，构建时由 Yocto recipe 或自行提供。

## 4. 待机音效（不在本仓库，随固件分发时适用）

- blip / swoosh / sparkle / boing：[Kenney](https://kenney.nl/assets)
  （interface-sounds、digital-audio 包）— **CC0**
- meow / bark / spring：[Mixkit](https://mixkit.co/free-sound-effects/)
  免费许可（sfx 92 / 741 / 736），条款见官网
