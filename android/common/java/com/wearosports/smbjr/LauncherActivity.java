package com.wearosports.smbjr;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Process;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.List;

/**
 * Entry point. Makes sure the user's ROM is imported before the SDL game activity starts.
 * Phone: system file picker. Watch (no picker on Wear OS): adb push into the app folder.
 */
public class LauncherActivity extends Activity {
    private static final int REQ_PICK = 4242;
    /** Phone app shortcut (long-press the icon): choose another ROM file even if one is imported. */
    public static final String ACTION_CHOOSE_ROM = "com.wearosports.smbjr.action.CHOOSE_ROM";
    private boolean choosing;  // opened through ACTION_CHOOSE_ROM
    private boolean launched;  // GameActivity already started (onActivityResult + onResume race)
    private TextView status;
    private RomInbox inbox;
    private RomTransferView transfer;

    static boolean isWatch(Activity a) {
        return a.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WATCH);
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        choosing = !isWatch(this) && getIntent() != null && ACTION_CHOOSE_ROM.equals(getIntent().getAction());
        if (choosing) {
            buildUi();
            return;
        }
        if (!tryStart()) {
            if (isWatch(this)) showTransfer();
            else buildUi();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!choosing && (status != null || transfer != null)) tryStart();
    }

    @Override
    protected void onDestroy() {
        if (inbox != null) inbox.stop();
        super.onDestroy();
    }

    /** Watch: Wi-Fi transfer screen (QR + address). adb push into dropDir() still works too. */
    private void showTransfer() {
        transfer = new RomTransferView(this);
        setContentView(transfer);
        inbox = new RomInbox(this, RomStore.dropDir(this), new RomInbox.Listener() {
            @Override public void onServerReady(String url, boolean[][] qr) { transfer.setQrCode(url, qr); }
            @Override public void onProgress(int percent) { transfer.setProgress(percent); }
            @Override public void onError(String msg) { transfer.setStatus(msg, 0xFFFF7366); }
            @Override public void onFileReceived(File file) {
                transfer.setStatus(RomTransferView.okText(), 0xFF66F28C);
                transfer.postDelayed(() -> {
                    if (!tryStart()) transfer.setStatus(RomTransferView.failText(), 0xFFFF7366);
                }, 600);
            }
        });
        inbox.start();
    }

    private boolean tryStart() {
        if (launched) return true;
        if (!RomStore.hasRom(this)) {
            File dropped = RomStore.findDropped(this);
            if (dropped != null) {
                try {
                    String warn = RomStore.importFrom(this, dropped);
                    if (warn != null) Toast.makeText(this, warn, Toast.LENGTH_LONG).show();
                    dropped.delete();
                } catch (Exception e) {
                    if (status != null) status.setText("Erro ao importar: " + e.getMessage());
                    dropped.delete();
                    return false;
                }
            }
        }
        if (!RomStore.hasRom(this)) {
            // Debug builds carry the ROM in the APK (like the FireRed port): install = playable.
            try {
                RomStore.importBundled(this);
            } catch (Exception e) {
                if (status != null) status.setText("Erro ao importar: " + e.getMessage());
            }
        }
        if (!RomStore.hasRom(this)) return false;
        // The game runs from its private copy (RomStore.romFile): copies sent over Wi-Fi or adb are not needed.
        RomStore.deleteDropped(this);
        if (inbox != null) { inbox.stop(); inbox = null; }
        launched = true;
        Intent game = new Intent(this, GameActivity.class);
        if (getIntent() != null && getIntent().getExtras() != null) game.putExtras(getIntent().getExtras());
        startActivity(game);
        finish();
        return true;
    }

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private Button button(String label, View.OnClickListener l) {
        Button btn = new Button(this);
        btn.setText(label);
        btn.setAllCaps(false);
        btn.setMinHeight(dp(48));
        btn.setOnClickListener(l);
        return btn;
    }

    private void buildUi() {
        boolean watch = isWatch(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = watch ? dp(28) : dp(24);
        col.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText(getApplicationInfo().loadLabel(getPackageManager()));
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(Color.rgb(250, 199, 46));
        title.setTextSize(watch ? 16 : 22);
        title.setGravity(Gravity.CENTER);
        col.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(watch ? 12 : 15);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(10), 0, dp(10));
        if (watch) {
            status.setText("Envie a sua ROM (.gba) para o relogio:\n\nadb push rom.gba "
                    + RomStore.dropDir(this).getAbsolutePath() + "/\n\nDepois toque em Verificar.");
        } else if (choosing && RomStore.hasRom(this)) {
            status.setText("Escolha outro arquivo .gba de Super Monkey Ball Jr. para substituir a ROM atual. "
                    + "O save continua o mesmo.");
        } else {
            status.setText("Selecione o arquivo .gba do seu cartucho de Super Monkey Ball Jr. (USA). "
                    + "A ROM fica so neste aparelho.\n\n"
                    + "Para trocar a ROM depois: toque e segure o icone do app > Trocar ROM.");
        }
        col.addView(status);

        if (!watch) {
            col.addView(button("Escolher ROM", v -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                startActivityForResult(i, REQ_PICK);
            }));
        }
        if (choosing && RomStore.hasRom(this)) {
            col.addView(button("Voltar ao jogo", v -> { choosing = false; tryStart(); }));
        } else {
            col.addView(button("Verificar novamente", v -> {
                if (!tryStart()) Toast.makeText(this, "ROM ainda nao encontrada", Toast.LENGTH_SHORT).show();
            }));
        }

        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(Color.BLACK);
        sv.setFillViewport(true);
        sv.addView(col);
        setContentView(sv);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_PICK || res != RESULT_OK || data == null || data.getData() == null) return;
        try {
            String err = RomStore.importFrom(this, data.getData());
            if (err != null) {
                // Rejected: the previous ROM (if any) is untouched; stay here so the user can pick again.
                if (status != null) status.setText(err);
                Toast.makeText(this, err, Toast.LENGTH_LONG).show();
                return;
            }
        } catch (Exception e) {
            if (status != null) status.setText("Erro ao importar: " + e.getMessage());
            return;
        }
        if (choosing) {
            // A game started with the old ROM may still be alive in the background: it runs in its
            // own process (":game"), so end it and start fresh with the new file.
            killGameProcess(this);
            choosing = false;
            Toast.makeText(this, "ROM trocada", Toast.LENGTH_SHORT).show();
            // Give the system a moment to reap the old process before the game starts again.
            getWindow().getDecorView().postDelayed(this::tryStart, 400);
            return;
        }
        tryStart();
    }

    /** Ends the ":game" process (phone manifest) so the native core restarts with the new ROM. */
    static void killGameProcess(Context c) {
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> procs = am != null ? am.getRunningAppProcesses() : null;
        if (procs == null) return;
        String game = c.getPackageName() + ":game";
        for (ActivityManager.RunningAppProcessInfo p : procs) {
            if (game.equals(p.processName) && p.pid != Process.myPid()) Process.killProcess(p.pid);
        }
    }
}
