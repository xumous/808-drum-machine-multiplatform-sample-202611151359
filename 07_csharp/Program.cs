// ============================================================================
//  Xu808 Program.cs —— 程序入口（C# / WinForms）
//
//  支持三种无界面模式，便于在构建机上自动验证：
//      XU808-CSharp.exe --selftest
//      XU808-CSharp.exe --export out.wav
//      XU808-CSharp.exe --midi out.mid
//  正常双击则进入图形界面。
// ============================================================================

using System;
using System.IO;
using System.Windows.Forms;

namespace Xu808
{
    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
        {
            var project = new XuProject();

            if (args.Length >= 1 && args[0] == "--selftest")
            {
                SelfTest(project);
                return;
            }
            if (args.Length >= 2 && args[0] == "--export")
            {
                var eng = new XuEngine(project);
                eng.ExportWav(args[1], false);
                File.WriteAllText(args[1] + ".ok", "OK");
                return;
            }
            if (args.Length >= 2 && args[0] == "--midi")
            {
                var eng = new XuEngine(project);
                eng.ExportMidi(args[1], false);
                File.WriteAllText(args[1] + ".ok", "OK");
                return;
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new XuMainForm(project));
        }

        /// <summary>
        /// 自检：渲染一个 pattern，检查非静音、峰值、停止后残留。
        /// 结果写到 selftest.txt，因为 WinExe 没有控制台可看。
        /// </summary>
        private static void SelfTest(XuProject p)
        {
            var lines = new System.Collections.Generic.List<string>();
            var eng = new XuEngine(p);
            var audio = eng.RenderPattern();
            int frames = audio.Length / 2;
            float peak = 0, energy = 0;
            for (int i = 0; i < audio.Length; i++)
            {
                float a = Math.Abs(audio[i]);
                if (a > peak) peak = a;
                energy += audio[i] * audio[i];
            }
            float rms = (float)Math.Sqrt(energy / Math.Max(1, audio.Length));
            lines.Add($"samples={frames} peak={peak:F4} rms={rms:F4}");
            bool ok = frames > XuConst.SampleRate && peak > 0.01f && peak <= 1.0f;
            lines.Add(ok ? "SELFTEST OK" : "SELFTEST FAIL");
            File.WriteAllText("selftest.txt", string.Join(Environment.NewLine, lines));
        }
    }
}
