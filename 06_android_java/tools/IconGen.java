// ============================================================================
//  IconGen.java —— 生成 XU-808 启动图标（PNG，多密度）
//  徐某：app.ico 是 Windows 图标格式，Java 不原生支持；这里用 Java2D 画一个
//  同风格的 808 图标（深底 + 橙色 808 + 青色步进条），输出到 res/mipmap-*。
// ============================================================================

import javax.imageio.ImageIO;
import java.awt.*;
import java.awt.geom.RoundRectangle2D;
import java.awt.image.BufferedImage;
import java.io.File;

public final class IconGen {
    public static void main(String[] args) throws Exception {
        String root = args.length > 0 ? args[0] : ".";
        int[][] targets = {
                {48, 0}, {72, 1}, {96, 2}, {144, 3}, {192, 4}
        };
        String[] dirs = {"mipmap-mdpi", "mipmap-hdpi", "mipmap-xhdpi",
                "mipmap-xxhdpi", "mipmap-xxxhdpi"};
        for (int[] t : targets) {
            int size = t[0];
            BufferedImage img = new BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB);
            Graphics2D g = img.createGraphics();
            g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
            g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING,
                    RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
            paint(g, size);
            g.dispose();
            File dir = new File(root, "res/" + dirs[t[1]]);
            dir.mkdirs();
            ImageIO.write(img, "png", new File(dir, "ic_launcher.png"));
            System.out.println("icon " + size + " -> " + dir.getPath());
        }
    }

    private static void paint(Graphics2D g, int s) {
        // 深色圆角底
        g.setColor(new Color(0xFF121212));
        g.fillRect(0, 0, s, s);
        RoundRectangle2D body = new RoundRectangle2D.Float(
                s * 0.05f, s * 0.05f, s * 0.90f, s * 0.90f, s * 0.22f, s * 0.22f);
        g.setColor(new Color(0xFF1C1C1C));
        g.fill(body);
        g.setColor(new Color(0xFFFF7A18));
        g.setStroke(new BasicStroke(Math.max(2f, s * 0.022f)));
        g.draw(body);

        // 顶部 XU 小字
        g.setColor(new Color(0xFF4CC9F0));
        g.setFont(new Font("SansSerif", Font.BOLD, Math.max(9, (int) (s * 0.13f))));
        String xu = "XU";
        FontMetrics fm = g.getFontMetrics();
        g.drawString(xu, (s - fm.stringWidth(xu)) / 2f, s * 0.28f);

        // 主字 808
        g.setColor(new Color(0xFFFF7A18));
        g.setFont(new Font("SansSerif", Font.BOLD, Math.max(14, (int) (s * 0.36f))));
        fm = g.getFontMetrics();
        String big = "808";
        g.drawString(big, (s - fm.stringWidth(big)) / 2f, s * 0.66f);

        // 底部 8 个步进块，表现鼓机网格
        float bw = s * 0.075f, bh = s * 0.075f, gap = s * 0.022f;
        float total = 8 * bw + 7 * gap;
        float x = (s - total) / 2f, y = s * 0.74f;
        for (int i = 0; i < 8; i++) {
            g.setColor(i == 1 ? new Color(0xFFFFD166) : new Color(0xFFFF7A18));
            g.fill(new RoundRectangle2D.Float(x + i * (bw + gap), y, bw, bh, bw * 0.3f, bh * 0.3f));
        }
    }
}
