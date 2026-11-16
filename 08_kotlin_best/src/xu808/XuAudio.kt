// ============================================================================
//  XuAudio.kt —— 独立音频线程（javax.sound.sampled）  ·  徐某
//
//  规格书要求：音频播放绝不能在 Swing 的事件线程（EDT）里做。
//  徐某因此起一条最高优先级的专用线程：
//      render() → 转 int16 → SourceDataLine.write()
//  线程里不碰任何 Swing 组件，也不做文件/网络 IO。
// ============================================================================
package xu808

import javax.sound.sampled.AudioFormat
import javax.sound.sampled.AudioSystem
import javax.sound.sampled.DataLine
import javax.sound.sampled.SourceDataLine

class XuAudio(private val engine: XuEngine) {

    private var line: SourceDataLine? = null
    private var thread: Thread? = null

    @Volatile
    private var running = false

    // 预先分配：音频线程里绝不 new 数组
    private val bytes = ByteArray(XuEngine.BLOCK * 4)   // 2 声道 × 2 字节
    private val L = FloatArray(XuEngine.BLOCK)
    private val R = FloatArray(XuEngine.BLOCK)

    /** 打开 48k/16bit/立体声输出并启动线程；没有声卡时返回 false（仍可离线导出）。 */
    fun start(): Boolean {
        try {
            val fmt = AudioFormat(48000f, 16, 2, true, false)
            val info = DataLine.Info(SourceDataLine::class.java, fmt)
            val l = AudioSystem.getLine(info) as SourceDataLine
            l.open(fmt, 8192)       // ≈ 2048 帧 ≈ 43ms 硬件缓冲
            l.start()
            line = l
        } catch (ex: Exception) {
            line = null
            return false
        }
        running = true
        thread = Thread({ loop() }, "XU808-Audio").also {
            it.priority = Thread.MAX_PRIORITY   // 尽可能接近实时
            it.isDaemon = true
            it.start()
        }
        return true
    }

    private fun loop() {
        val line = this.line ?: return
        while (running) {
            val t0 = System.nanoTime()
            try {
                engine.render(XuEngine.BLOCK, L, R)
                var p = 0
                for (i in 0 until XuEngine.BLOCK) {
                    val l = (Math.max(-1f, Math.min(1f, L[i])) * 32767f).toInt()
                    val r = (Math.max(-1f, Math.min(1f, R[i])) * 32767f).toInt()
                    bytes[p++] = (l and 0xFF).toByte()
                    bytes[p++] = ((l shr 8) and 0xFF).toByte()
                    bytes[p++] = (r and 0xFF).toByte()
                    bytes[p++] = ((r shr 8) and 0xFF).toByte()
                }
                // 只统计“合成 + 量化”耗时；line.write 会阻塞等缓冲，不能算进 CPU
                val dt = System.nanoTime() - t0
                line.write(bytes, 0, p)
                val ideal = XuEngine.BLOCK / 48000.0
                engine.cpu = engine.cpu * 0.95f + ((dt / 1e9) / ideal).toFloat() * 0.05f
            } catch (ex: Exception) {
                // 设备被拔掉之类：记一次 xrun 并退出，UI 仍能继续工作
                engine.xruns = engine.xruns + 1
                break
            }
        }
    }

    /** 关闭窗口时调用：先停线程，再冲掉缓冲，保证没有残留进程/爆音。 */
    fun stop() {
        running = false
        thread?.let {
            try {
                it.join(500)
            } catch (ignored: InterruptedException) {
            }
        }
        thread = null
        line?.let {
            try {
                it.stop()
                it.flush()
                it.close()
            } catch (ignored: Exception) {
            }
        }
        line = null
    }
}
