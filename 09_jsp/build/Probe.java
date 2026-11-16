// 临时探针：用 Java 引擎以 128 样本块渲染同一工程，验证“块大小”是否会影响结果。
package xu808;

public class Probe {
    public static void main(String[] args) throws Exception {
        XuProject p = XuProject.load(args[0]);
        XuEngine e = new XuEngine(p);
        int total = 240000;
        float[] L = new float[128], R = new float[128];
        float[] out = new float[total * 2];
        e.play();
        int done = 0;
        while (done < total) {
            int n = Math.min(128, total - done);
            e.render(n, L, R);
            for (int i = 0; i < n; i++) {
                out[(done + i) * 2] = L[i];
                out[(done + i) * 2 + 1] = R[i];
            }
            done += n;
        }
        float peak = 0, sum = 0;
        for (float v : out) {
            float a = Math.abs(v);
            if (a > peak) peak = a;
            sum += v * v;
        }
        System.out.printf("peak=%.6f rms=%.6f s5632=%.6f s0=%.6f%n",
                peak, Math.sqrt(sum / out.length), out[5632 * 2], out[0]);
        XuEngine.writeWav(args[1], out);
    }
}
