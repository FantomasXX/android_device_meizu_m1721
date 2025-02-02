/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.settings.mback;
import android.content.Context;
import android.util.Log;
import android.view.KeyEvent;
import android.os.Vibrator;
import android.os.VibrationEffect;
import android.os.ServiceManager;
import android.app.Fragment;
import android.app.Activity;
import android.provider.Settings;
import android.media.AudioManager;
import org.lineageos.settings.mback.MBackSettings;
import org.lineageos.settings.preferences.FileUtils;
import java.lang.Integer;

import com.android.internal.os.DeviceKeyHandler;

public class KeyHandler implements DeviceKeyHandler {
    private static final String TAG = "KeyHandler";
    private Context mContext;
    private Vibrator mVibrator;
    private AudioManager mAudioManager;
    private long prevTime;

    private int getVibrationData() {
        return Settings.Secure.getInt(mContext.getContentResolver(), MBackSettings.KEY_VIBRO_STRENGTH, 110);
    }

    private void setVibrationData(int data) {
        Settings.Secure.putInt(mContext.getContentResolver(), MBackSettings.KEY_VIBRO_STRENGTH, data);
    }

    private int getTouchData() {
        return Settings.Secure.getInt(mContext.getContentResolver(), MBackSettings.KEY_TOUCH_SOUND, 10);
    }

    private void setTouchData(int data) {
        Settings.Secure.putInt(mContext.getContentResolver(), MBackSettings.KEY_TOUCH_SOUND, data);
    }

    private void mBackPlaySound() {
        if (mAudioManager.getRingerModeInternal() == AudioManager.RINGER_MODE_NORMAL) {
            int settingsVolume = getTouchData();
            int maxVolume = mAudioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
            float realVolume = settingsVolume / maxVolume;

            mAudioManager.playSoundEffect(AudioManager.FX_FOCUS_NAVIGATION_DOWN, realVolume);
        }
    }

    private void mBackVibrate() {
        int duration = getVibrationData();
        if (duration <= 0) {
            Log.i(TAG, "mBack vibration is disabled");
            return;
        }

        VibrationEffect effect = VibrationEffect.createOneShot(duration, VibrationEffect.DEFAULT_AMPLITUDE);

        if (mVibrator != null)
            mVibrator.vibrate(effect);
        else
            Log.e(TAG, "mVibrator is null");
    }

    public KeyEvent handleKeyEvent(KeyEvent event) {
        if (event.getScanCode() == 158 && event.getAction() == KeyEvent.ACTION_DOWN && prevTime != event.getEventTime()) {
            // Avoid double handling
            prevTime = event.getEventTime();

            mBackVibrate();
            mBackPlaySound();
        }

        return event;
    }

    public KeyHandler(Context context) {
        mVibrator = context.getSystemService(Vibrator.class);
        mAudioManager = context.getSystemService(AudioManager.class);
        mContext = context;
    }
}
