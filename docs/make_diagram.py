# -*- coding: utf-8 -*-
"""生成 README 用的架构示意图（深色 808 风格）。"""
from PIL import Image, ImageDraw, ImageFont

W, H = 1280, 640
BG = (18, 18, 18)
PANEL = (35, 35, 35)
ORANGE = (255, 122, 24)
CYAN = (76, 201, 240)
TEXT = (232, 232, 232)
DIM = (150, 150, 150)

img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)
font_path = r"C:\Windows\Fonts\msyh.ttc"
try:
    f_big = ImageFont.truetype(font_path, 34)
    f_mid = ImageFont.truetype(font_path, 22)
    f_small = ImageFont.truetype(font_path, 17)
except Exception:
    f_big = f_mid = f_small = ImageFont.load_default()


def box(x, y, w, h, title, lines, color):
    d.rounded_rectangle([x, y, x + w, y + h], radius=10, fill=PANEL, outline=color, width=3)
    d.text((x + 18, y + 12), title, font=f_mid, fill=color)
    yy = y + 46
    for ln in lines:
        d.text((x + 18, yy), ln, font=f_small, fill=TEXT)
        yy += 24


d.text((40, 26), "XU-808 架构与线程模型", font=f_big, fill=ORANGE)
d.text((42, 74), "UI 线程只编辑数据与刷新画面；音频回调以样本计数为唯一时钟。", font=f_small, fill=DIM)

box(50, 140, 300, 150, "UI 线程", ["Qt / Swing / WinForms / DOM", "30fps 读取快照", "绝不驱动音序"], ORANGE)
box(490, 120, 300, 190, "Project 数据", ["Pattern / Step / Pad", "Master / Song", "普通字段，原子读写"], CYAN)
box(490, 380, 300, 190, "Engine 引擎", ["samplePos 唯一时钟", "事件 → 绝对样本", "Voice 池 / Choke / 抢占"], ORANGE)
box(930, 380, 300, 190, "音频回调线程", ["waveOut / QAudioSink", "零分配 / 零锁", "48kHz / 256~512 帧"], CYAN)
box(930, 140, 300, 150, "主效果", ["HP → LP → Drive", "软限幅 → 音量", "左右独立 biquad"], CYAN)


# 箭头
def arrow(x1, y1, x2, y2):
    d.line([x1, y1, x2, y2], fill=DIM, width=3)
    d.polygon([(x2, y2), (x2 - 10, y2 - 6), (x2 - 10, y2 + 6)], fill=DIM)


arrow(350, 215, 490, 215)  # UI -> Project
arrow(640, 310, 640, 380)  # Project -> Engine
arrow(790, 475, 930, 475)  # Engine -> Audio
arrow(930, 215, 790, 215)  # Master <- Audio? draw separate
arrow(790, 215, 930, 215)
d.text((360, 190), "写参数 / 画格子", font=f_small, fill=DIM)
d.text((652, 330), "只读", font=f_small, fill=DIM)
d.text((800, 445), "每块渲染", font=f_small, fill=DIM)
d.text((800, 188), "主效果链", font=f_small, fill=DIM)

d.text((50, 596), "署名：徐某 · 所有声音实时合成，无采样 · 上一个音自然结束或被 choke 释放，绝不叠加",
       font=f_small, fill=DIM)
img.save("architecture.png")
print("saved architecture.png")
