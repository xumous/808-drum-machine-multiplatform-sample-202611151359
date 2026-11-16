// ============================================================================
//  XuGridView.java —— 16 声部 × N 步的自定义网格（MVC 中的 View）
//
//  徐某说明：
//    · 用 Canvas 手绘，不依赖 RecyclerView/AndroidX，一个 View 画完 16×64；
//    · 左侧声部名与 M/S 固定在原地，只有右侧格子随手指横向滚动；
//    行数放不下时还能纵向拖动（16 行 × 25dp 在矮屏上也能操作）；
//    · 单击格子 = 开/关并试听；长按格子 = 打开步进属性；长按声部名 = 轨道长度；
//    · 播放头用青色描边，每帧由 Activity 的 UI 定时器触发 invalidate。
// ============================================================================
package com.xumou.xu808;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;

public class XuGridView extends View {

    // 配色（规格书第 9 节）
    private static final int C_PANEL = 0xFF1C1C1C;
    private static final int C_LINE = 0xFF2E2E2E;
    private static final int C_CELL = 0xFF232323;
    private static final int C_CELL_DIM = 0xFF1A1A1A;
    private static final int C_ACCENT = 0xFFFF7A18;
    private static final int C_ACCENT2 = 0xFFFFD166;
    private static final int C_PLAY = 0xFF4CC9F0;
    private static final int C_TXT = 0xFFE8E8E8;
    private static final int C_DIM = 0xFF8A8A8A;
    private static final int C_MUTE = 0xFF9B3232;
    private static final int C_SOLO = 0xFF8A6A1F;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textBold = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final float density;
    private final int labelW, headerH, rowH, cellW, gap, slop;
    private XuProject proj;
    private XuEngine engine;
    private Listener listener;
    private int selectedPad = 0;
    private int scrollX = 0, scrollY = 0, maxScrollX = 0, maxScrollY = 0;
    private float downX, downY, lastX, lastY;
    private boolean dragging, longFired;
    /**
     * 长按判定：480ms 内没有移动就当作长按。
     */
    private final Runnable longPress = new Runnable() {
        @Override
        public void run() {
            if (dragging || longFired || listener == null) return;
            longFired = true;
            fireLongPress(downX, downY);
        }
    };

    public XuGridView(Context ctx, XuProject p, XuEngine e) {
        super(ctx);
        proj = p;
        engine = e;
        density = getResources().getDisplayMetrics().density;
        labelW = (int) dp(96);
        headerH = (int) dp(19);
        rowH = (int) dp(25);
        cellW = (int) dp(33);
        gap = (int) dp(3);
        slop = ViewConfiguration.get(ctx).getScaledTouchSlop();

        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(dp(1));
        text.setTextSize(dp(8.5f));
        text.setColor(C_TXT);
        textBold.setTextSize(dp(9.5f));
        textBold.setColor(C_TXT);
        textBold.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));

        setFocusable(true);
        setClickable(true);
    }

    private static int withAlpha(int color, int alpha) {
        return (color & 0x00FFFFFF) | ((alpha & 0xFF) << 24);
    }

    private float dp(float v) {
        return v * density;
    }

    public void setListener(Listener l) {
        listener = l;
    }

    public void setProject(XuProject p, XuEngine e) {
        proj = p;
        engine = e;
        scrollX = 0;
        scrollY = 0;
        invalidate();
    }

    public int getSelectedPad() {
        return selectedPad;
    }

    public void setSelectedPad(int pad) {
        selectedPad = Math.max(0, Math.min(15, pad));
        invalidate();
    }

    // ------------------------------------------------------------------
    // 绘制
    // ------------------------------------------------------------------
    @Override
    protected void onDraw(Canvas c) {
        c.drawColor(C_PANEL);
        if (proj == null) return;

        XuProject.Pattern pat = proj.cur();
        int steps = Math.max(1, Math.min(XuProject.MAX_STEPS, pat.steps));
        int w = getWidth(), h = getHeight();

        // 先更新滚动范围，保证旋转/改步数后不会滚出界
        int contentW = steps * cellW;
        maxScrollX = Math.max(0, contentW - (w - labelW));
        maxScrollY = Math.max(0, XuProject.PADS * rowH - (h - headerH));
        clampScroll();

        // ---- 顶部步号条 ----
        fill.setColor(0xFF171717);
        c.drawRect(labelW, 0, w, headerH, fill);
        c.save();
        c.clipRect(labelW, 0, w, headerH);
        c.translate(-scrollX, 0);
        for (int s = 0; s < steps; s++) {
            float x = labelW + s * cellW + cellW * 0.5f;
            boolean beat = (s % 4) == 0;
            text.setColor(beat ? C_TXT : C_DIM);
            text.setTextSize(dp(8f));
            text.setTextAlign(Paint.Align.CENTER);
            c.drawText(String.valueOf(s + 1), x, headerH - dp(5), text);
        }
        c.restore();

        // 左上角“声部”标题
        text.setColor(C_DIM);
        text.setTextSize(dp(8f));
        text.setTextAlign(Paint.Align.LEFT);
        c.drawText("声部 / M S", dp(5), headerH - dp(5), text);

        // ---- 格子区（横向 + 纵向都随手指滚动） ----
        c.save();
        c.clipRect(labelW, headerH, w, h);
        c.translate(-scrollX, headerH - scrollY);
        int playStep = engine != null && engine.playing ? engine.getStepIndex() : -1;

        for (int pad = 0; pad < XuProject.PADS; pad++) {
            float cy = pad * rowH;
            // 选中行做一条淡淡的底色
            if (pad == selectedPad) {
                fill.setColor(0xFF242424);
                c.drawRect(labelW, cy, labelW + contentW, cy + rowH, fill);
            }
            int tl = Math.max(1, Math.min(XuProject.MAX_STEPS, pat.trackLen[pad]));
            for (int s = 0; s < steps; s++) {
                XuProject.Step st = pat.data[pad][s];
                float l = labelW + s * cellW + gap * 0.5f;
                float t = cy + gap * 0.5f;
                float r = l + cellW - gap;
                float b = t + rowH - gap;

                boolean beyond = s >= tl;      // 超出该轨长度：变暗，提示 polymeter
                rect.set(l, t, r, b);

                if (st.on) {
                    int base = st.accent ? C_ACCENT2 : C_ACCENT;
                    int alpha = 90 + (int) (st.velocity / 127f * 150f);
                    fill.setColor(withAlpha(base, Math.min(255, alpha)));
                    c.drawRoundRect(rect, dp(3), dp(3), fill);
                    // 重音加一圈亮边，远看更醒目
                    if (st.accent) {
                        stroke.setColor(withAlpha(C_ACCENT2, 220));
                        c.drawRoundRect(rect, dp(3), dp(3), stroke);
                    }
                } else {
                    fill.setColor(beyond ? C_CELL_DIM : C_CELL);
                    c.drawRoundRect(rect, dp(3), dp(3), fill);
                    stroke.setColor(beyond ? 0xFF262626 : C_LINE);
                    stroke.setStrokeWidth(dp(0.8f));
                    c.drawRoundRect(rect, dp(3), dp(3), stroke);
                }

                // 概率 <100：右上角一个小方点；ratchet>1：底部小圆点
                if (st.on && st.prob < 100) {
                    fill.setColor(0xFF0E0E0E);
                    c.drawCircle(r - dp(3.5f), t + dp(3.5f), dp(1.4f), fill);
                }
                if (st.on && st.ratchet > 1) {
                    fill.setColor(0xFF0E0E0E);
                    for (int k = 0; k < Math.min(4, st.ratchet); k++) {
                        c.drawCircle(l + dp(4) + k * dp(4), b - dp(4), dp(1.2f), fill);
                    }
                }
                // 微时：左侧/右侧画一根小竖线（正值在右，负值在左）
                if (st.on && st.micro != 0) {
                    stroke.setColor(0xFF0E0E0E);
                    stroke.setStrokeWidth(dp(1.2f));
                    float mx = st.micro > 0 ? r - dp(3) : l + dp(3);
                    c.drawLine(mx, t + dp(3), mx, b - dp(3), stroke);
                }
            }

        }

        // 播放头：整列描边（必须最后画，避免被后续行覆盖）
        if (playStep >= 0 && playStep < steps) {
            float l = labelW + playStep * cellW + gap * 0.5f;
            rect.set(l, 0, l + cellW - gap, XuProject.PADS * rowH);
            stroke.setColor(C_PLAY);
            stroke.setStrokeWidth(dp(1.6f));
            c.drawRoundRect(rect, dp(3), dp(3), stroke);
        }
        c.restore();

        // ---- 左侧固定声部名 + M/S ----
        c.save();
        c.clipRect(0, headerH, labelW, h);
        c.translate(0, headerH - scrollY);
        for (int pad = 0; pad < XuProject.PADS; pad++) {
            XuProject.Pad p = proj.pads[pad];
            float cy = pad * rowH;
            if (pad == selectedPad) {
                fill.setColor(0xFF2A2A2A);
                c.drawRect(0, cy, labelW, cy + rowH, fill);
                fill.setColor(C_ACCENT);
                c.drawRect(0, cy, dp(2), cy + rowH, fill);
            }
            // 声部代号（橙色）+ 中文名（灰）
            textBold.setColor(pad == selectedPad ? C_ACCENT : C_TXT);
            textBold.setTextSize(dp(9f));
            textBold.setTextAlign(Paint.Align.LEFT);
            c.drawText(XuProject.CODES[pad], dp(6), cy + rowH * 0.44f, textBold);
            text.setColor(C_DIM);
            text.setTextSize(dp(7.5f));
            c.drawText(XuProject.NAMES_CN[pad], dp(23), cy + rowH * 0.42f, text);
            // 轨道长度放第二行，避免和 M/S 按钮重叠
            text.setColor(0xFF6A6A6A);
            text.setTextSize(dp(6.5f));
            c.drawText("循环 " + pat.trackLen[pad], dp(6), cy + rowH * 0.86f, text);

            // M / S 小按钮
            drawSquareButton(c, labelW - dp(45), cy + dp(4), dp(18), dp(18),
                    "M", p.mute, C_MUTE);
            drawSquareButton(c, labelW - dp(23), cy + dp(4), dp(18), dp(18),
                    "S", p.solo, C_SOLO);
        }
        c.restore();

        // 顶部分隔线
        fill.setColor(C_LINE);
        c.drawRect(0, headerH - dp(1), w, headerH, fill);
        fill.setColor(C_LINE);
        c.drawRect(labelW - dp(1), 0, labelW, h, fill);
    }

    /**
     * 画一个 M/S 小方块按钮。
     */
    private void drawSquareButton(Canvas c, float x, float y, float w, float h,
                                  String label, boolean on, int onColor) {
        rect.set(x, y, x + w, y + h);
        fill.setColor(on ? onColor : 0xFF2A2A2A);
        c.drawRoundRect(rect, dp(2), dp(2), fill);
        text.setColor(on ? Color.WHITE : 0xFF8A8A8A);
        text.setTextSize(dp(8f));
        text.setTextAlign(Paint.Align.CENTER);
        c.drawText(label, x + w * 0.5f, y + h * 0.70f, text);
    }

    private void clampScroll() {
        if (scrollX < 0) scrollX = 0;
        if (scrollX > maxScrollX) scrollX = maxScrollX;
        if (scrollY < 0) scrollY = 0;
        if (scrollY > maxScrollY) scrollY = maxScrollY;
    }

    // ------------------------------------------------------------------
    // 触摸：拖动滚动 / 单击 / 长按
    // ------------------------------------------------------------------
    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = lastX = ev.getX();
                downY = lastY = ev.getY();
                dragging = false;
                longFired = false;
                handler.postDelayed(longPress, 480);
                return true;

            case MotionEvent.ACTION_MOVE: {
                float dx = ev.getX() - lastX;
                float dy = ev.getY() - lastY;
                if (!dragging && (Math.abs(ev.getX() - downX) > slop
                        || Math.abs(ev.getY() - downY) > slop)) {
                    dragging = true;
                    handler.removeCallbacks(longPress);
                }
                if (dragging) {
                    scrollX -= (int) dx;
                    scrollY -= (int) dy;
                    clampScroll();
                    lastX = ev.getX();
                    lastY = ev.getY();
                    invalidate();
                }
                return true;
            }

            case MotionEvent.ACTION_UP:
                handler.removeCallbacks(longPress);
                if (!dragging && !longFired) handleTap(downX, downY);
                return true;

            case MotionEvent.ACTION_CANCEL:
                handler.removeCallbacks(longPress);
                return true;
            default:
                return super.onTouchEvent(ev);
        }
    }

    /**
     * 屏幕坐标 → 行号；越界返回 -1。
     */
    private int padAt(float y) {
        if (y < headerH) return -1;
        int pad = (int) ((y - headerH + scrollY) / rowH);
        if (pad < 0 || pad >= XuProject.PADS) return -1;
        // 该行必须完整落在可视区内，避免点到被裁掉的半行
        if (headerH + (pad + 1) * rowH - scrollY > getHeight()) return -1;
        return pad;
    }

    private void handleTap(float x, float y) {
        if (listener == null) return;
        int pad = padAt(y);
        if (pad < 0) return;
        if (x < labelW) {
            // 先判断 M / S 按钮
            if (x >= labelW - dp(45) && x <= labelW - dp(27)) {
                listener.onMuteToggle(pad);
            } else if (x >= labelW - dp(23) && x <= labelW - dp(5)) {
                listener.onSoloToggle(pad);
            } else {
                selectedPad = pad;
                invalidate();
                listener.onPadSelected(pad);
                listener.onPadAudition(pad);   // 只有点声部名才试听
            }
            return;
        }
        int step = (int) ((x - labelW + scrollX) / cellW);
        if (step < 0 || step >= proj.cur().steps) return;
        selectedPad = pad;
        invalidate();
        listener.onPadSelected(pad);
        listener.onStepTap(pad, step);
    }

    private void fireLongPress(float x, float y) {
        int pad = padAt(y);
        if (pad < 0) return;
        if (x < labelW) {
            // 长按声部名：编辑该轨长度（polymeter）
            if (x < labelW - dp(45)) listener.onTrackLenLongPress(pad);
        } else {
            int step = (int) ((x - labelW + scrollX) / cellW);
            if (step < 0 || step >= proj.cur().steps) return;
            selectedPad = pad;
            invalidate();
            listener.onStepLongPress(pad, step, x, y);
        }
    }

    /**
     * 事件回调：由 Controller（MainActivity）实现，View 不直接改数据。
     */
    public interface Listener {
        void onStepTap(int pad, int step);

        void onStepLongPress(int pad, int step, float x, float y);

        void onPadSelected(int pad);

        /**
         * 点击左侧声部名时试听一次（与“选中”分开，避免点格子时重复发声）。
         */
        void onPadAudition(int pad);

        void onMuteToggle(int pad);

        void onSoloToggle(int pad);

        void onTrackLenLongPress(int pad);
    }
}
