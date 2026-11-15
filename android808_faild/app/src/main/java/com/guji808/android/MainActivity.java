package com.guji808.android;

import android.app.Activity;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;

public class MainActivity extends Activity {
    private long engine;
    private AudioEngine808 audio;
    private MidiInput midi;
    private MachineView view;
    private File projectDir;
    private File exportDir;
    private AudioManager audioManager;
    private AudioFocusRequest focusRequest;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        hideSystemUi();

        File baseDir = getExternalFilesDir(null);
        if (baseDir == null) baseDir = getFilesDir();
        projectDir = new File(baseDir, "projects");
        exportDir = new File(baseDir, "exports");
        if (!projectDir.exists()) projectDir.mkdirs();
        if (!exportDir.exists()) exportDir.mkdirs();

        engine = Native808.create(48000.0);
        audio = new AudioEngine808(engine);
        midi = new MidiInput(this, engine);

        view = new MachineView(this, engine, audio, midi, projectDir, exportDir);
        setContentView(view);

        audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (Build.VERSION.SDK_INT >= 26) {
            AudioAttributes attrs = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build();
            focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(attrs)
                    .setOnAudioFocusChangeListener(change -> {
                        if (change <= AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) {
                            if (audio.isRunning()) audio.stop();
                        } else if (!audio.isRunning()) {
                            audio.start();
                        }
                    })
                    .build();
        }
    }

    private void hideSystemUi() {
        View decor = getWindow().getDecorView();
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = decor.getWindowInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                c.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            decor.setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUi();
        if (audio != null) audio.start();
        if (view != null) view.onResume();
        if (audioManager != null && focusRequest != null) {
            audioManager.requestAudioFocus(focusRequest);
        }
    }

    @Override
    protected void onPause() {
        if (audio != null) audio.stop();
        if (audioManager != null && focusRequest != null) {
            audioManager.abandonAudioFocusRequest(focusRequest);
        }
        if (view != null) view.onPause();
        super.onPause();
    }

    @Override
    public void onBackPressed() {
        if (view != null && view.handleBack()) return;
        super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        if (view != null) view.onPause();
        if (midi != null) midi.close();
        if (audio != null) audio.stop();
        if (engine != 0 && (view == null || !view.hasActiveWorkers())) {
            Native808.destroy(engine);
            engine = 0;
        }
        super.onDestroy();
    }
}
