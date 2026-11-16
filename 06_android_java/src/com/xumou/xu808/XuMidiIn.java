// ============================================================================
//  XuMidiIn.java —— Android MIDI 输入（规格书第 8 节）
//
//  徐某说明：Android 没有系统级 MIDI 端口，USB/BLE MIDI 设备通过 MidiManager 接入。
//  本类做的事：
//    · 监听 MIDI 设备热插拔，自动打开第一个可用设备；
//    · 解析 Note On（触发声部）、CC7/74/71、0xF8 时钟、0xFA/0xFC、Program Change；
//    · 所有回调都在主线程 Handler 上落到引擎队列，音频线程只消费队列。
//  没有 MIDI 设备时本类完全静默，不影响鼓机本体。
// ============================================================================
package com.xumou.xu808;

import android.content.Context;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiInputPort;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Handler;
import android.os.Looper;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

public final class XuMidiIn {
    private final XuEngine engine;
    private final XuProject proj;
    private final MidiManager manager;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final List<MidiDevice> devices = new ArrayList<>();
    private final List<MidiOutputPort> ports = new ArrayList<>();
    private final MidiManager.DeviceCallback deviceCallback = new MidiManager.DeviceCallback() {
        @Override
        public void onDeviceAdded(MidiDeviceInfo info) {
            openDevice(info);
        }

        @Override
        public void onDeviceRemoved(MidiDeviceInfo info) { /* 端口断开由系统回收 */ }
    };
    private volatile boolean clockSeen = false;
    // 实时时钟超时：800ms 收不到 0xF8 就交还内部时钟，避免“卡死”
    private final Runnable clockTimeout = new Runnable() {
        @Override
        public void run() {
            clockSeen = false;
            engine.clockExternal = false;
        }
    };

    public XuMidiIn(Context ctx, XuEngine e, XuProject p) {
        engine = e;
        proj = p;
        manager = (MidiManager) ctx.getSystemService(Context.MIDI_SERVICE);
    }

    /**
     * 把一个 MIDI 音符映射到声部编号；找不到返回 -1。
     */
    private static int padOf(int note) {
        for (int i = 0; i < XuProject.MIDI_NOTES.length; i++)
            if (XuProject.MIDI_NOTES[i] == note) return i;
        return -1;
    }

    private static float clamp01(float v) {
        return v < 0 ? 0 : (v > 1 ? 1 : v);
    }

    /**
     * 打开所有带输出端口的设备；失败不抛异常（部分厂商实现不完整）。
     */
    public void start() {
        if (manager == null) return;
        try {
            manager.registerDeviceCallback(deviceCallback, handler);
        } catch (Throwable ignored) {
        }
        try {
            MidiDeviceInfo[] infos = manager.getDevices();
            for (MidiDeviceInfo info : infos) openDevice(info);
        } catch (Throwable ignored) {
        }
    }

    private void openDevice(final MidiDeviceInfo info) {
        if (manager == null) return;
        if (info.getOutputPortCount() <= 0) return;   // 该设备不发送 MIDI
        try {
            manager.openDevice(info, new MidiManager.OnDeviceOpenedListener() {
                @Override
                public void onDeviceOpened(MidiDevice device) {
                    if (device == null) return;
                    devices.add(device);
                    // 打开第一个输出端口接收事件
                    try {
                        MidiOutputPort port = device.openOutputPort(0);
                        if (port != null) {
                            port.connect(new Receiver());
                            ports.add(port);
                        }
                    } catch (Throwable ignored) {
                    }
                }
            }, handler);
        } catch (Throwable ignored) {
        }
    }

    /**
     * 关闭所有端口/设备，Activity 销毁时调用。
     */
    public void close() {
        handler.removeCallbacks(clockTimeout);
        if (manager != null) {
            try {
                manager.unregisterDeviceCallback(deviceCallback);
            } catch (Throwable ignored) {
            }
        }
        for (MidiOutputPort p : ports) {
            try {
                p.close();
            } catch (Throwable ignored) {
            }
        }
        ports.clear();
        for (MidiDevice d : devices) {
            try {
                d.close();
            } catch (Throwable ignored) {
            }
        }
        devices.clear();
        engine.clockExternal = false;
    }

    /**
     * 处理一条完整 MIDI 消息（不含系统实时字节）。
     */
    private void handle(int status, int d1, int d2) {
        int type = status & 0xF0;
        switch (type) {
            case 0x90: {  // Note On
                if (d2 > 0) {
                    int pad = padOf(d1);
                    if (pad >= 0) engine.triggerPad(pad, d2, d2 >= 110);
                }
                break;
            }
            case 0x80:    // Note Off：鼓机单击即响，忽略
                break;
            case 0xB0: {  // Control Change
                if (d1 == 7) {                 // 主音量
                    proj.master.volume = clamp01(d2 / 127f);
                } else if (d1 == 74) {         // 低通截止：0–127 → 200–20000Hz 指数
                    proj.master.lpCutoff = (float) (200.0 * Math.pow(100.0, d2 / 127.0));
                    proj.master.lpOn = true;
                } else if (d1 == 71) {         // 共鸣 Q：0.5–8
                    proj.master.lpQ = (float) (0.5 + d2 / 127.0 * 7.5);
                }
                break;
            }
            case 0xC0: {  // Program Change：切换 Pattern（0–63 钳到 0–15）
                engine.requestPattern(Math.max(0, Math.min(15, d1)));
                break;
            }
            default:
                break;
        }
    }

    /**
     * 接收器：支持 running status 与系统实时字节插队。
     */
    private final class Receiver extends MidiReceiver {
        private int status = 0;
        private int need = 0;
        private int count = 0;
        private int d1 = 0;

        @Override
        public void onSend(byte[] msg, int offset, int n, long timestamp) throws IOException {
            for (int i = 0; i < n; i++) {
                int b = msg[offset + i] & 0xFF;

                // 系统实时字节可以出现在任何位置
                if (b >= 0xF8) {
                    switch (b) {
                        case 0xF8: {                       // 时钟：24 PPQN
                            clockSeen = true;
                            if (!engine.clockExternal) engine.clockExternal = true;
                            engine.externalTick();
                            handler.removeCallbacks(clockTimeout);
                            handler.postDelayed(clockTimeout, 800);
                            break;
                        }
                        case 0xFA:
                            engine.play();
                            break;    // Start
                        case 0xFC:
                            engine.stop();
                            break;    // Stop
                        case 0xFB:
                            engine.play();
                            break;    // Continue（简化处理）
                        default:
                            break;
                    }
                    continue;
                }
                if (b >= 0x80) {                        // 状态字节
                    status = b;
                    int t = b & 0xF0;
                    need = (t == 0xC0 || t == 0xD0) ? 1 : 2;
                    count = 0;
                    d1 = 0;
                    continue;
                }
                if (status == 0) continue;              // 没有状态字节，丢弃
                if (need == 1) {
                    handle(status, b, 0);
                } else if (count == 0) {
                    d1 = b;
                    count = 1;
                } else {
                    handle(status, d1, b);
                    count = 0;
                }
            }
        }
    }
}
