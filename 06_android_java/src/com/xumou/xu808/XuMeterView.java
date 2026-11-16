// ============================================================================
//  XuMeterView.java —— 主输出双声道电平表（MVC 中的 View）
//  徐某：只读引擎的 meterL/meterR 快照，绝不反过来影响音频线程。
// ============================================================================
package com.xumou.xu808;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.View;

public class XuMeterView extends View {
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float density;
    private volatile float left = 0, right = 0;

    public XuMeterView(Context ctx) {
        super(ctx);
        density = getResources().getDisplayMetrics().density;
    }

    private static int withAlpha(int color, int a) {
        return (color & 0x00FFFFFF) | ((a & 0xFF) << 24);
    }

    /**
     * 由 UI 定时器每帧写入（0–1 峰值）。
     */
    public void setLevels(float l, float r) {
        left = Math.max(0, Math.min(1, l));
        right = Math.max(0, Math.min(1, r));
        invalidate();
    }

    @Override
    protected void onDraw(Canvas c) {
        c.drawColor(0xFF161616);
        float w = getWidth(), h = getHeight();
        float barH = (h - dp(3)) * 0.5f;
        drawBar(c, dp(1), dp(1), w - dp(2), barH, left);
        drawBar(c, dp(1), dp(1) + barH + dp(1), w - dp(2), barH, right);
    }

    private void drawBar(Canvas c, float x, float y, float w, float h, float level) {
        // 底槽
        p.setColor(0xFF101010);
        c.drawRect(x, y, x + w, y + h, p);
        // 20 段 LED：绿 → 黄 → 红
        int seg = 20;
        int lit = (int) Math.round(level * seg);
        float sw = w / seg;
        for (int i = 0; i < seg; i++) {
            float frac = (i + 1f) / seg;
            int col = frac > 0.88f ? 0xFFFF4040 : frac > 0.70f ? 0xFFFFD166 : 0xFF3DDC84;
            p.setColor(i < lit ? col : withAlpha(col, 0x28));
            c.drawRect(x + i * sw + dp(0.7f), y,
                    x + (i + 1) * sw - dp(0.7f), y + h, p);
        }
    }

    private float dp(float v) {
        return v * density;
    }
}
