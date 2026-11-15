package com.guji808.android;

import android.content.Context;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Handler;
import android.os.Looper;

/**
 * android.media.midi 输入：把外部 MIDI 音符/时钟/走带事件通过
 * core808 的无锁 MIDI 队列（c808_push_midi）送入音频线程。
 */
public final class MidiInput {
    private final MidiManager manager;
    private final long engine;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private MidiDevice device;
    private MidiOutputPort port;
    private String name = "MIDI: OFF";
    private boolean opening;

    public MidiInput(Context ctx, long engine) {
        this.manager = (MidiManager) ctx.getSystemService(Context.MIDI_SERVICE);
        this.engine = engine;
    }

    public String status() {
        return name;
    }

    public boolean isOpen() {
        return device != null;
    }

    public boolean isAvailable() {
        return manager != null;
    }

    /**
     * 依次尝试打开设备；再调用一次则关闭。
     */
    public void toggle() {
        if (device != null || opening) {
            close();
            return;
        }
        if (manager == null) {
            name = "MIDI: NOT SUPPORTED";
            return;
        }
        MidiDeviceInfo[] infos = manager.getDevices();
        if (infos == null || infos.length == 0) {
            name = "MIDI: NO DEVICE";
            return;
        }
        MidiDeviceInfo target = null;
        for (MidiDeviceInfo i : infos) {
            if (i.getOutputPortCount() > 0) {
                target = i;
                break;
            }
        }
        if (target == null) {
            name = "MIDI: NO INPUT PORT";
            return;
        }
        final MidiDeviceInfo info = target;
        opening = true;
        name = "MIDI: OPENING...";
        manager.openDevice(info, new MidiManager.OnDeviceOpenedListener() {
            @Override
            public void onDeviceOpened(MidiDevice d) {
                opening = false;
                if (d == null) {
                    name = "MIDI: OPEN FAILED";
                    return;
                }
                device = d;
                try {
                    port = d.openOutputPort(0);
                    if (port == null) {
                        name = "MIDI: PORT FAILED";
                        close();
                        return;
                    }
                    port.connect(new MidiReceiver() {
                        @Override
                        public void onSend(byte[] msg, int offset, int count, long timestamp) {
                            dispatch(msg, offset, count);
                        }
                    });
                    name = "MIDI: " + info.getProperties()
                            .getString(MidiDeviceInfo.PROPERTY_NAME, "DEVICE");
                } catch (Exception ex) {
                    name = "MIDI: ERROR";
                    close();
                }
            }
        }, handler);
    }

    private void dispatch(byte[] msg, int offset, int count) {
        if (engine == 0) return;
        int i = offset;
        int end = offset + count;
        while (i < end) {
            int status = msg[i] & 0xff;
            if (status >= 0xF8) {
                if (status == 0xF8) Native808.pushMidi(engine, 4, 0, 0, 0);
                else if (status == 0xFA) Native808.pushMidi(engine, 5, 0, 0, 0);
                else if (status == 0xFC) Native808.pushMidi(engine, 6, 0, 0, 0);
                else if (status == 0xFB) Native808.pushMidi(engine, 7, 0, 0, 0);
                i++;
                continue;
            }
            int type = status & 0xF0;
            int dataLen = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            if (i + dataLen >= end) break;
            int d1 = msg[i + 1] & 0x7f;
            int d2 = dataLen > 1 ? (msg[i + 2] & 0x7f) : 0;
            if (type == 0x90 && d2 > 0) {
                Native808.pushMidi(engine, 1, d1, d2, 0);
            } else if (type == 0xC0) {
                Native808.pushMidi(engine, 3, d1, 0, 0);
            }
            i += dataLen + 1;
        }
    }

    public void close() {
        try {
            if (port != null) port.close();
        } catch (Exception ignored) {
        }
        port = null;
        try {
            if (device != null) device.close();
        } catch (Exception ignored) {
        }
        device = null;
        name = "MIDI: OFF";
    }
}
