package com.wearosports.smbjr;

import android.content.pm.PackageManager;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** SDL3 host for the native game (libmain.so: mGBA core + shell). */
public class GameActivity extends SDLActivity {

    // Watch: the screen never rotates. SDL asks for a sensor-driven orientation that follows
    // the game's window shape; ignore it and pin portrait (rotation 0 on a round/square watch;
    // "nosensor" would keep whatever rotation the lock screen left behind).
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        if (!isWatchDevice()) super.setOrientationBis(w, h, resizable, hint);
    }

    @Override
    public void setRequestedOrientation(int requestedOrientation) {
        super.setRequestedOrientation(isWatchDevice()
                ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT : requestedOrientation);
    }

    private boolean isWatchDevice() {
        return getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_WATCH);
    }

    private boolean watch;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        args.add(RomStore.romFile(this).getAbsolutePath());
        // Extra "rect" (testing): force the rectangular phone layout even on a round/watch device.
        boolean forceRect = getIntent() != null && getIntent().getBooleanExtra("rect", false);
        if (!forceRect && (watch || getResources().getConfiguration().isScreenRound())) args.add("--round");
        return args.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        watch = getPackageManager().hasSystemFeature(PackageManager.FEATURE_WATCH);
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Swipes are game input: keep edge gestures from minimizing the game.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            final View decor = getWindow().getDecorView();
            decor.post(() -> decor.setSystemGestureExclusionRects(
                    Collections.singletonList(new Rect(0, 0, decor.getWidth(), decor.getHeight()))));
        }
    }

    /** Rotary crown / bezel -> SDL mouse wheel (native: zoom). */
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        if (ev.getAction() == MotionEvent.ACTION_SCROLL && ev.isFromSource(InputDevice.SOURCE_ROTARY_ENCODER)) {
            float delta = -ev.getAxisValue(MotionEvent.AXIS_SCROLL);
            if (delta != 0) {
                SDLActivity.onNativeMouse(0, MotionEvent.ACTION_SCROLL, 0, delta > 0 ? 1 : -1, false);
            }
            return true;
        }
        return super.dispatchGenericMotionEvent(ev);
    }
}
