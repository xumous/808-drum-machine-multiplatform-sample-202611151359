# -*- coding: utf-8 -*-
"""
gen_docs.py —— 把 docs/*.md 生成为同名 .docx 与 .ppt(x)。

徐某写这个脚本，是为了让七种语言的 README 保持“同源”：
只维护 Markdown，三份交付物自动生成，避免手改 docx 漏掉一段。
"""
import os
import re
from docx import Document
from docx.shared import Pt, RGBColor, Inches
from docx.enum.text import WD_ALIGN_PARAGRAPH
from pptx import Presentation
from pptx.util import Inches as PInches, Pt as PPt
from pptx.dml.color import RGBColor as PRGB

HERE = os.path.dirname(os.path.abspath(__file__))

ORANGE = RGBColor(0xFF, 0x7A, 0x18)
P_ORANGE = PRGB(0xFF, 0x7A, 0x18)


def parse(md_path):
    """把 Markdown 拆成 (title, [(heading, [blocks])])；块类型为 p/bullet/code。"""
    with open(md_path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    title = ""
    sections = []
    cur = ("", [])
    in_code = False
    code = []
    for line in lines:
        if line.startswith("```"):
            if in_code:
                cur[1].append(("code", "\n".join(code)))
                code = []
                in_code = False
            else:
                in_code = True
            continue
        if in_code:
            code.append(line)
            continue
        if line.startswith("# "):
            title = line[2:].strip()
        elif line.startswith("## "):
            if cur[0] or cur[1]:
                sections.append(cur)
            cur = (line[3:].strip(), [])
        elif line.startswith("### "):
            cur[1].append(("h3", line[4:].strip()))
        elif line.strip().startswith(("- ", "* ")):
            cur[1].append(("bullet", line.strip()[2:].strip()))
        elif re.match(r"^\d+\.\s", line.strip()):
            cur[1].append(("bullet", line.strip()))
        elif line.strip() == "---":
            continue
        elif line.strip():
            cur[1].append(("p", line.strip()))
    if cur[0] or cur[1]:
        sections.append(cur)
    return title, sections


def make_docx(title, sections, out_path):
    doc = Document()
    style = doc.styles["Normal"]
    style.font.name = "Microsoft YaHei"
    style.font.size = Pt(10.5)
    h = doc.add_heading(title, level=0)
    for run in h.runs:
        run.font.color.rgb = ORANGE
    png = os.path.join(HERE, "architecture.png")
    if os.path.exists(png):
        doc.add_picture(png, width=Inches(6.2))
        doc.add_paragraph("图 1：XU-808 的线程与模块调用关系（徐某绘制）")
    for head, blocks in sections:
        if head:
            hh = doc.add_heading(head, level=1)
            for run in hh.runs:
                run.font.color.rgb = ORANGE
        for kind, text in blocks:
            if kind == "p":
                doc.add_paragraph(text)
            elif kind == "bullet":
                doc.add_paragraph(text, style="List Bullet")
            elif kind == "h3":
                h3 = doc.add_heading(text, level=2)
                for run in h3.runs:
                    run.font.color.rgb = ORANGE
            elif kind == "code":
                p = doc.add_paragraph()
                r = p.add_run(text)
                r.font.name = "Consolas"
                r.font.size = Pt(8.5)
                p.paragraph_format.left_indent = Inches(0.2)
    doc.save(out_path)


def make_pptx(title, sections, out_path):
    prs = Presentation()
    prs.slide_width = PInches(13.333)
    prs.slide_height = PInches(7.5)
    blank = prs.slide_layouts[6]

    # 封面
    s = prs.slides.add_slide(blank)
    tb = s.shapes.add_textbox(PInches(0.8), PInches(2.4), PInches(11.7), PInches(1.6))
    p = tb.text_frame.paragraphs[0]
    p.text = title
    p.font.size = PPt(36)
    p.font.bold = True
    p.font.color.rgb = P_ORANGE
    sub = s.shapes.add_textbox(PInches(0.85), PInches(4.0), PInches(11.5), PInches(1.0))
    sp = sub.text_frame.paragraphs[0]
    sp.text = "XU-808 · 16 voices · real-time synthesis"
    sp.font.size = PPt(16)
    sp.font.color.rgb = PRGB(0xB0, 0xB0, 0xB0)

    # 第二页：架构图
    png = os.path.join(HERE, "architecture.png")
    if os.path.exists(png):
        s2 = prs.slides.add_slide(blank)
        s2.shapes.add_picture(png, PInches(0.8), PInches(0.5), width=PInches(11.7))

    for head, blocks in sections:
        s = prs.slides.add_slide(blank)
        t = s.shapes.add_textbox(PInches(0.6), PInches(0.35), PInches(12.1), PInches(0.8))
        tp = t.text_frame.paragraphs[0]
        tp.text = head or "XU-808"
        tp.font.size = PPt(26)
        tp.font.bold = True
        tp.font.color.rgb = P_ORANGE

        body = s.shapes.add_textbox(PInches(0.65), PInches(1.25), PInches(12.0), PInches(5.9))
        tf = body.text_frame
        tf.word_wrap = True
        first = True
        for kind, text in blocks:
            if kind == "code":
                # 代码太长只展示前几行，完整内容在 docx/md 里
                text = "\n".join(text.splitlines()[:8])
            if len(text) > 260:
                text = text[:257] + "…"
            para = tf.paragraphs[0] if first else tf.add_paragraph()
            first = False
            para.text = ("• " if kind == "bullet" else "") + text
            para.font.size = PPt(13 if kind != "code" else 10)
            para.font.color.rgb = PRGB(0xE8, 0xE8, 0xE8)
            para.space_after = PPt(5)
    prs.save(out_path)


def main():
    for name in sorted(os.listdir(HERE)):
        if not name.endswith(".md") or name.lower() == "readme.md":
            continue
        path = os.path.join(HERE, name)
        title, sections = parse(path)
        base = os.path.splitext(path)[0]
        make_docx(title, sections, base + ".docx")
        # python-pptx 只能写 OOXML；先写 .pptx 保证格式正确，
        # 再复制一份 .ppt 满足命名要求（现代 PowerPoint 可正常打开）。
        make_pptx(title, sections, base + ".pptx")
        with open(base + ".pptx", "rb") as src, open(base + ".ppt", "wb") as dst:
            dst.write(src.read())
        print("generated", base + ".docx", base + ".pptx", base + ".ppt")


if __name__ == "__main__":
    main()
