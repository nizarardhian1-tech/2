package com.hybrid.imgui;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.List;

public class MainActivity extends Activity {

    // ── UI refs ───────────────────────────────────────────────────────────────
    private TextView     tvTitle, tvStatus, tvGameSelected, tvLog;
    private Button       btnScan, btnInject, btnStop, btnPermission;
    private ListView     listGames;
    private ScrollView   logScroll;

    // ── State ─────────────────────────────────────────────────────────────────
    private List<GameDetector.GameInfo> gameList;
    private GameDetector.GameInfo       selectedGame;
    private Injector                    injector;
    private ModManager                  modManager;
    private Handler                     mainHandler;
    private StringBuilder               logBuffer = new StringBuilder();

    // ── Colors ────────────────────────────────────────────────────────────────
    private static final int BG_DARK    = 0xFF0D0D0D;
    private static final int BG_CARD    = 0xFF1A1A2E;
    private static final int NEON_CYAN  = 0xFF00F5FF;
    private static final int NEON_GREEN = 0xFF39FF14;
    private static final int NEON_RED   = 0xFFFF2D55;
    private static final int TEXT_WHITE = 0xFFEEEEEE;
    private static final int TEXT_GRAY  = 0xFF888888;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        mainHandler = new Handler(Looper.getMainLooper());
        injector    = new Injector(this, msg -> appendLog(msg));

        // ModManager mengelola injector binary lifecycle
        modManager  = new ModManager(this);
        modManager.setListener(new ModManager.StatusListener() {
            @Override public void onLog(String msg)           { appendLog(msg); }
            @Override public void onStateChanged(boolean run) {
                mainHandler.post(() -> {
                    setButtonEnabled(btnInject, !run);
                    setButtonEnabled(btnStop,    run);
                });
            }
        });

        buildUI();
        checkRoot();
        checkOverlayPermission();
    }

    // =========================================================================
    // BUILD UI — fully programmatic, no XML
    // =========================================================================
    private void buildUI() {
        // Root container
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG_DARK);
        root.setPadding(dp(12), dp(40), dp(12), dp(12));

        // ── Title bar ─────────────────────────────────────────────────────────
        LinearLayout titleBar = row();
        titleBar.setBackgroundColor(BG_CARD);
        titleBar.setPadding(dp(16), dp(12), dp(16), dp(12));

        tvTitle = label("⬡  IL2Cpp Hybrid Tool", 18, NEON_CYAN, true);
        tvStatus = label("○ Checking root...", 12, TEXT_GRAY, false);
        tvStatus.setGravity(Gravity.END);

        titleBar.addView(tvTitle, wrapWeight(1));
        titleBar.addView(tvStatus, wrapWrap());
        root.addView(titleBar, matchWrap());

        space(root, 8);

        // ── Permission warning (hidden by default) ─────────────────────────
        btnPermission = button("⚠  Grant Overlay Permission", NEON_RED, 0xFF330000);
        btnPermission.setVisibility(View.GONE);
        btnPermission.setOnClickListener(v -> requestOverlayPermission());
        root.addView(btnPermission, matchWrap());

        space(root, 4);

        // ── Selected game indicator ────────────────────────────────────────
        tvGameSelected = label("No game selected — tap Scan to detect Unity games", 12, TEXT_GRAY, false);
        tvGameSelected.setPadding(dp(8), dp(8), dp(8), dp(8));
        tvGameSelected.setBackgroundColor(BG_CARD);
        root.addView(tvGameSelected, matchWrap());

        space(root, 8);

        // ── Buttons row ────────────────────────────────────────────────────
        LinearLayout btnRow = row();
        btnScan   = button("⟳  Scan Games",  NEON_CYAN,  0xFF001A2E);
        btnInject = button("⚡  Inject",      NEON_GREEN, 0xFF001A00);
        btnStop   = button("■  Stop",         NEON_RED,   0xFF1A0000);

        btnInject.setEnabled(false);
        btnInject.setAlpha(0.4f);
        btnStop.setEnabled(false);
        btnStop.setAlpha(0.4f);

        btnScan.setOnClickListener   (v -> scanGames());
        btnInject.setOnClickListener (v -> startInject());
        btnStop.setOnClickListener   (v -> stopAll());

        btnRow.addView(btnScan,   wrapWeight(1));
        space(btnRow, 6);
        btnRow.addView(btnInject, wrapWeight(1));
        space(btnRow, 6);
        btnRow.addView(btnStop,   wrapWeight(1));
        root.addView(btnRow, matchWrap());

        space(root, 8);

        // ── Game list ──────────────────────────────────────────────────────
        TextView listLabel = label("Detected Unity Games:", 11, TEXT_GRAY, false);
        root.addView(listLabel, matchWrap());
        space(root, 4);

        listGames = new ListView(this);
        listGames.setBackgroundColor(BG_CARD);
        listGames.setDividerHeight(1);
        listGames.setDivider(null);
        listGames.setOnItemClickListener((av, v, pos, id) -> {
            selectedGame = gameList.get(pos);
            tvGameSelected.setText("Selected: " + selectedGame.appName +
                "\n" + selectedGame.packageName);
            tvGameSelected.setTextColor(NEON_CYAN);
            setButtonEnabled(btnInject, true);
            setButtonEnabled(btnStop,   true);
        });
        LinearLayout.LayoutParams listParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(180));
        root.addView(listGames, listParams);

        space(root, 8);

        // ── Log section ────────────────────────────────────────────────────
        TextView logLabel = label("Log:", 11, TEXT_GRAY, false);
        root.addView(logLabel, matchWrap());
        space(root, 4);

        logScroll = new ScrollView(this);
        logScroll.setBackgroundColor(0xFF050505);

        tvLog = new TextView(this);
        tvLog.setTextColor(0xFF00FF88);
        tvLog.setTextSize(10);
        tvLog.setTypeface(Typeface.MONOSPACE);
        tvLog.setPadding(dp(8), dp(8), dp(8), dp(8));
        tvLog.setText("Ready.\n");

        logScroll.addView(tvLog, matchWrap());
        LinearLayout.LayoutParams logParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(logScroll, logParams);

        setContentView(root);
    }

    // =========================================================================
    // ACTIONS
    // =========================================================================
    private void checkRoot() {
        new Thread(() -> {
            boolean root = Injector.hasRoot();
            mainHandler.post(() -> {
                if (root) {
                    tvStatus.setText("● ROOT OK");
                    tvStatus.setTextColor(NEON_GREEN);
                } else {
                    tvStatus.setText("✗ NO ROOT");
                    tvStatus.setTextColor(NEON_RED);
                    appendLog("ERROR: Root access not available!");
                    appendLog("This tool requires root (Magisk/KernelSU).");
                }
            });
        }).start();
    }

    private void checkOverlayPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (!Settings.canDrawOverlays(this)) {
                btnPermission.setVisibility(View.VISIBLE);
                appendLog("⚠ Overlay permission needed for UI display");
            }
        }
    }

    private void requestOverlayPermission() {
        Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
            Uri.parse("package:" + getPackageName()));
        startActivityForResult(intent, 1001);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == 1001) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M &&
                Settings.canDrawOverlays(this)) {
                btnPermission.setVisibility(View.GONE);
                appendLog("✓ Overlay permission granted");
            }
        }
    }

    private void scanGames() {
        appendLog("Scanning for Unity games...");
        btnScan.setEnabled(false);

        new Thread(() -> {
            gameList = GameDetector.detectUnityGames(this);
            mainHandler.post(() -> {
                btnScan.setEnabled(true);
                if (gameList.isEmpty()) {
                    appendLog("No Unity/IL2Cpp games found.");
                    appendLog("Make sure game is installed.");
                } else {
                    appendLog("Found " + gameList.size() + " Unity game(s):");
                    for (GameDetector.GameInfo g : gameList) {
                        appendLog("  • " + g.appName + " — " + g.packageName);
                    }
                }

                // Update ListView
                String[] names = new String[gameList.size()];
                for (int i = 0; i < gameList.size(); i++) {
                    names[i] = gameList.get(i).appName + "\n" + gameList.get(i).packageName;
                }
                ArrayAdapter<String> adapter = new ArrayAdapter<String>(
                    this, android.R.layout.simple_list_item_1, names) {
                    @Override
                    public View getView(int pos, View cv, ViewGroup parent) {
                        TextView tv = (TextView) super.getView(pos, cv, parent);
                        tv.setTextColor(TEXT_WHITE);
                        tv.setBackgroundColor(pos % 2 == 0 ? BG_CARD : 0xFF1F1F3A);
                        tv.setPadding(dp(12), dp(10), dp(12), dp(10));
                        return tv;
                    }
                };
                listGames.setAdapter(adapter);
            });
        }).start();
    }

    private void startInject() {
        if (selectedGame == null) {
            Toast.makeText(this, "Select a game first", Toast.LENGTH_SHORT).show();
            return;
        }

        // Cek overlay permission
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M &&
            !Settings.canDrawOverlays(this)) {
            Toast.makeText(this, "Overlay permission required!", Toast.LENGTH_LONG).show();
            requestOverlayPermission();
            return;
        }

        appendLog("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        setButtonEnabled(btnInject, false);

        final GameDetector.GameInfo game = selectedGame;

        new Thread(() -> {
            // Step 1: Injector siapkan libinternal.so + wrap-script (jika perlu)
            injector.fullInject(game);

            // Dapatkan path aktual libinternal.so yang sudah di-extract oleh Injector.java
            // Injector.java selalu extract ke /data/local/tmp/libinternal.so
            // Path ini yang harus dikirim ke binary injector (bukan nativeLibraryDir)
            String soPath = injector.getExtractedSoPath();
            log("soPath: " + soPath);

            modManager.launch(game.packageName, soPath);

            mainHandler.post(() -> setButtonEnabled(btnInject, true));
        }).start();
    }

    private void stopAll() {
        new Thread(() -> {
            modManager.stop();
            injector.stopAll();
        }).start();
    }

    // =========================================================================
    // LOG
    // =========================================================================
    private void appendLog(String msg) {
        mainHandler.post(() -> {
            logBuffer.append(msg).append("\n");
            // Keep only last 200 lines
            String s = logBuffer.toString();
            String[] lines = s.split("\n");
            if (lines.length > 200) {
                StringBuilder nb = new StringBuilder();
                for (int i = lines.length - 200; i < lines.length; i++)
                    nb.append(lines[i]).append("\n");
                logBuffer = nb;
            }
            tvLog.setText(logBuffer.toString());
            logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
        });
    }

    // =========================================================================
    // UI HELPERS
    // =========================================================================
    private int dp(int px) {
        float d = getResources().getDisplayMetrics().density;
        return (int)(px * d + 0.5f);
    }

    private TextView label(String text, int sp, int color, boolean bold) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(sp);
        tv.setTextColor(color);
        if (bold) tv.setTypeface(null, Typeface.BOLD);
        return tv;
    }

    private Button button(String text, int textColor, int bgColor) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextColor(textColor);
        b.setBackgroundColor(bgColor);
        b.setTextSize(12);
        b.setPadding(dp(8), dp(10), dp(8), dp(10));
        b.setAllCaps(false);
        return b;
    }

    private LinearLayout row() {
        LinearLayout ll = new LinearLayout(this);
        ll.setOrientation(LinearLayout.HORIZONTAL);
        ll.setGravity(Gravity.CENTER_VERTICAL);
        return ll;
    }

    private void space(ViewGroup parent, int dp) {
        View v = new View(this);
        parent.addView(v, new LinearLayout.LayoutParams(dp(dp), dp(dp)));
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams wrapWrap() {
        return new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams wrapWeight(float w) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
            ViewGroup.LayoutParams.WRAP_CONTENT, w);
        return lp;
    }

    private void setButtonEnabled(Button b, boolean enabled) {
        b.setEnabled(enabled);
        b.setAlpha(enabled ? 1f : 0.4f);
    }
}
