package uk.lgl;

// =============================================================================
// OverlayMain.java — Full Java overlay, PERSIS arsitektur MLBB-Mod
//
// TIDAK ADA libimgui_ext.so, tidak ada SurfaceView, tidak ada EGL/native.
// Menu dibuat 100% dari Android View API (Java).
//
// Flow:
//   app_process / CLASSPATH=APK / uk.lgl.OverlayMain
//   args[0] = gamePkg → SHM path = /data/data/<gamePkg>/files/tool_esp.shm
//   Poll SHM sampai connect → launch overlay window
//   OverlayESP (Canvas)   → draw ESP dari SHM data
//   Menu Java (Toggle/SeekBar) → tulis config ke SHM → dibaca libinternal
// =============================================================================

import android.content.Context;
import android.content.ContextWrapper;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.text.Html;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.lang.reflect.Method;

import static android.view.ViewGroup.LayoutParams.MATCH_PARENT;
import static android.view.ViewGroup.LayoutParams.WRAP_CONTENT;
import static android.widget.RelativeLayout.ALIGN_PARENT_RIGHT;

public class OverlayMain {

    static final int TYPE_OVERLAY = 2038; // TYPE_APPLICATION_OVERLAY

    // =========================================================================
    // ENTRY POINT — dipanggil oleh app_process
    // =========================================================================
    public static void main(String[] args) {
        Thread.setDefaultUncaughtExceptionHandler((t, e) ->
            System.err.println("[OverlayMain] CRASH: " + e));

        Looper.prepareMainLooper();

        // OOM adj — jangan di-kill
        try {
            int pid = android.os.Process.myPid();
            Runtime.getRuntime().exec(new String[]{
                "su", "-c", "echo -1000 > /proc/" + pid + "/oom_score_adj"
            });
        } catch (Exception e) {}

        final Context ctx = getContextReflection();
        if (ctx == null) { System.err.println("[OverlayMain] ctx null!"); return; }

        // args[0] = gamePkg dikirim dari LaunchOverlay() di injector
        final String gamePkg = (args != null && args.length > 0 && args[0] != null) ? args[0] : "";
        System.err.println("[OverlayMain] gamePkg=" + gamePkg);

        final SHMBridge shm = new SHMBridge(gamePkg);
        System.err.println("[OverlayMain] SHM path=" + shm.getPath());

        final Handler mainHandler = new Handler(Looper.getMainLooper());

        // Poll SHM — sama persis MLBB-Mod: overlay tidak nunggu library dimuat,
        // cukup nunggu file SHM ada + version match
        new Thread(() -> {
            boolean connected = false;
            for (int i = 0; i < 120 && !connected; i++) {
                connected = shm.connect();
                if (!connected) try { Thread.sleep(1000); } catch (Exception ignored) {}
            }
            final boolean ok = connected;
            System.err.println("[OverlayMain] SHM connected=" + ok);
            mainHandler.post(() -> new OverlayMain().startOverlay(ctx, ok ? shm : null));
        }).start();

        Looper.loop();
    }

    private static Context getContextReflection() {
        try {
            Class<?> c = Class.forName("android.app.ActivityThread");
            Method sm = c.getDeclaredMethod("systemMain"); sm.setAccessible(true);
            Object at = sm.invoke(null);
            Method gs = c.getDeclaredMethod("getSystemContext"); gs.setAccessible(true);
            return (Context) gs.invoke(at);
        } catch (Exception e) {}
        return null;
    }

    // =========================================================================
    // DESIGN TOKENS
    // =========================================================================
    static final int C_BG_WINDOW   = 0xFAF8F9FB;
    static final int C_BG_SCROLL   = 0xF5F2F4F8;
    static final int C_BG_CATEGORY = 0xFFF0F3F8;
    static final int C_ACCENT      = 0xFF1A73E8;
    static final int C_ACCENT_LT   = 0x1A1A73E8;
    static final int C_TEXT_MAIN   = 0xFF2C3E50;
    static final int C_TEXT_DIM    = 0xFF8A9BB0;
    static final int C_TEXT_ACCENT = 0xFF1A73E8;
    static final int C_ON          = 0xFF1A73E8;
    static final int C_OFF         = 0xFFE0E6EE;
    static final int C_DIVIDER     = 0xFFDDE3EC;

    static final int MENU_W  = 300;
    static final int MENU_H  = 220;
    static final int ICON_SZ = 40;

    // ── Feature list ──────────────────────────────────────────────────────────
    private static final String[] FEATURES = {
        "Category_ESP",
        "1_Toggle_Line ESP",
        "2_Toggle_Box ESP",
        "3_Toggle_Health Bar",
        "4_Toggle_Names",
        "5_Toggle_Distance",
        "Category_VISUAL",
        "6_Toggle_Show FPS",
        "-7_SeekBar_Camera FOV_0_50",
        "Category_COLOR",
        "-8_Spinner_Color_Red,White,Blue,Yellow,Cyan,Green",
    };

    // =========================================================================
    // STATE
    // =========================================================================
    private WindowManager wm;
    private WindowManager.LayoutParams espParams, menuParams;
    private FrameLayout    rootFrame;
    private RelativeLayout mCollapsed, mRootContainer;
    private LinearLayout   mExpanded, patches;
    private ScrollView     scrollView;
    private OverlayESP     espView;
    private SHMBridge      mShm;
    private TextView       mRestoreDot;

    // =========================================================================
    // SAFE CONTEXT
    // =========================================================================
    private static Context makeSafeCtx(Context base) {
        return new ContextWrapper(base) {
            @Override public Object getSystemService(String name) {
                if (AUDIO_SERVICE.equals(name) || ACCESSIBILITY_SERVICE.equals(name)
                        || VIBRATOR_SERVICE.equals(name)) return null;
                try { return super.getSystemService(name); } catch (Exception e) { return null; }
            }
        };
    }

    private static void safe(View v) {
        v.setSoundEffectsEnabled(false);
        v.setHapticFeedbackEnabled(false);
        v.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
    }

    // =========================================================================
    // START OVERLAY
    // =========================================================================
    private void startOverlay(Context ctx, SHMBridge shm) {
        mShm = shm;
        ctx  = makeSafeCtx(ctx);
        wm   = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);

        // ── ESP Canvas layer ──────────────────────────────────────────────────
        espView   = new OverlayESP(ctx, shm);
        espParams = new WindowManager.LayoutParams(
            MATCH_PARENT, MATCH_PARENT, TYPE_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
            | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT);
        espParams.gravity = Gravity.TOP | Gravity.START;
        if (Build.VERSION.SDK_INT >= 28) espParams.layoutInDisplayCutoutMode = 1;
        try { wm.addView(espView, espParams); } catch (Exception e) {}

        // ── Menu layer ────────────────────────────────────────────────────────
        initMenu(ctx);

        // Revive loop — pastikan view tidak hilang
        final Handler revive = new Handler(Looper.getMainLooper());
        revive.post(new Runnable() {
            @Override public void run() {
                try { if (!espView.isAttachedToWindow())   wm.addView(espView,   espParams); } catch (Exception e) {}
                try { if (!rootFrame.isAttachedToWindow()) wm.addView(rootFrame, menuParams); } catch (Exception e) {}
                revive.postDelayed(this, 1500);
            }
        });
    }

    // =========================================================================
    // INIT MENU — pure Java, persis MLBB-Mod style
    // =========================================================================
    private void initMenu(final Context ctx) {
        rootFrame      = new FrameLayout(ctx);
        mRootContainer = new RelativeLayout(ctx);
        mCollapsed     = new RelativeLayout(ctx);
        mCollapsed.setVisibility(View.VISIBLE);

        mExpanded = new LinearLayout(ctx);
        mExpanded.setVisibility(View.GONE);
        mExpanded.setOrientation(LinearLayout.VERTICAL);
        mExpanded.setBackgroundColor(C_BG_WINDOW);
        mExpanded.setLayoutParams(new LinearLayout.LayoutParams(dp(ctx, MENU_W), WRAP_CONTENT));

        // ── Floating icon ─────────────────────────────────────────────────────
        int iconPx = dp(ctx, ICON_SZ);
        final TextView iconView = new TextView(ctx);
        iconView.setLayoutParams(new RelativeLayout.LayoutParams(iconPx, iconPx));
        iconView.setBackgroundColor(C_ACCENT);
        iconView.setText("E");
        iconView.setTextColor(Color.WHITE);
        iconView.setGravity(Gravity.CENTER);
        iconView.setTypeface(Typeface.DEFAULT_BOLD);
        iconView.setTextSize(16f);
        iconView.setOnClickListener(v -> {
            mCollapsed.setVisibility(View.GONE);
            mExpanded.setVisibility(View.VISIBLE);
        });
        iconView.setOnTouchListener(makeDrag());
        safe(iconView);
        mCollapsed.addView(iconView);

        // ── Restore dot ───────────────────────────────────────────────────────
        mRestoreDot = new TextView(ctx);
        RelativeLayout.LayoutParams rdp = new RelativeLayout.LayoutParams(dp(ctx,18), dp(ctx,18));
        rdp.addRule(RelativeLayout.ALIGN_PARENT_RIGHT);
        rdp.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
        mRestoreDot.setLayoutParams(rdp);
        mRestoreDot.setBackgroundColor(C_ACCENT);
        mRestoreDot.setVisibility(View.GONE);
        mRestoreDot.setOnClickListener(v -> {
            mRestoreDot.setVisibility(View.GONE);
            mCollapsed.setVisibility(View.VISIBLE);
        });
        safe(mRestoreDot);

        // ── Accent top bar ────────────────────────────────────────────────────
        View topBar = new View(ctx);
        topBar.setLayoutParams(new LinearLayout.LayoutParams(MATCH_PARENT, dp(ctx,3)));
        topBar.setBackgroundColor(C_ACCENT);
        mExpanded.addView(topBar);

        // ── Title ─────────────────────────────────────────────────────────────
        RelativeLayout titleBar = new RelativeLayout(ctx);
        titleBar.setPadding(dp(ctx,16), dp(ctx,12), dp(ctx,12), dp(ctx,10));
        titleBar.setBackgroundColor(C_BG_WINDOW);

        LinearLayout titleRow = new LinearLayout(ctx);
        titleRow.setOrientation(LinearLayout.HORIZONTAL);
        titleRow.setGravity(Gravity.CENTER_VERTICAL);
        RelativeLayout.LayoutParams rlC = new RelativeLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT);
        rlC.addRule(RelativeLayout.CENTER_HORIZONTAL);
        rlC.addRule(RelativeLayout.CENTER_VERTICAL);
        titleRow.setLayoutParams(rlC);

        TextView tvT = new TextView(ctx); tvT.setText("ESP"); tvT.setTextColor(0xFF0D1117);
        tvT.setTypeface(Typeface.DEFAULT_BOLD); tvT.setTextSize(19f);
        TextView tvP = new TextView(ctx); tvP.setText("+"); tvP.setTextColor(C_ACCENT);
        tvP.setTypeface(Typeface.DEFAULT_BOLD); tvP.setTextSize(17f);
        tvP.setPadding(0, dp(ctx,3), 0, 0);
        titleRow.addView(tvT); titleRow.addView(tvP);
        titleBar.addView(titleRow);
        mExpanded.addView(titleBar);
        mExpanded.addView(divider(ctx));

        // ── Scroll area ───────────────────────────────────────────────────────
        scrollView = new ScrollView(ctx);
        scrollView.setLayoutParams(new LinearLayout.LayoutParams(MATCH_PARENT, dp(ctx, MENU_H)));
        scrollView.setBackgroundColor(C_BG_SCROLL);

        patches = new LinearLayout(ctx);
        patches.setOrientation(LinearLayout.VERTICAL);
        patches.setPadding(0, dp(ctx,4), 0, dp(ctx,4));
        scrollView.addView(patches);
        mExpanded.addView(scrollView);
        mExpanded.addView(divider(ctx));

        // ── Bottom bar ────────────────────────────────────────────────────────
        LinearLayout btnBar = new LinearLayout(ctx);
        btnBar.setOrientation(LinearLayout.HORIZONTAL);
        btnBar.setPadding(dp(ctx,14), dp(ctx,6), dp(ctx,14), dp(ctx,7));
        btnBar.setBackgroundColor(C_BG_WINDOW);
        btnBar.setGravity(Gravity.CENTER_VERTICAL);

        TextView ver = new TextView(ctx); ver.setText("v4.0");
        ver.setTextColor(C_TEXT_DIM); ver.setTextSize(9f);
        ver.setLayoutParams(new LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f));

        final TextView hideBtn = new TextView(ctx); hideBtn.setText("HIDE");
        hideBtn.setTextColor(C_TEXT_DIM); hideBtn.setTypeface(Typeface.DEFAULT_BOLD);
        hideBtn.setTextSize(11f); hideBtn.setPadding(dp(ctx,10), dp(ctx,4), dp(ctx,10), dp(ctx,4));
        hideBtn.setOnClickListener(v -> {
            mCollapsed.setVisibility(View.GONE);
            mExpanded.setVisibility(View.GONE);
            mRestoreDot.setVisibility(View.VISIBLE);
        });
        hideBtn.setOnLongClickListener(v -> {
            try { wm.removeView(rootFrame); } catch (Exception e) {}
            try { wm.removeView(espView);   } catch (Exception e) {}
            System.exit(0);
            return true;
        });
        safe(hideBtn);

        final TextView minBtn = new TextView(ctx); minBtn.setText("MIN");
        minBtn.setTextColor(C_ACCENT); minBtn.setTypeface(Typeface.DEFAULT_BOLD);
        minBtn.setTextSize(11f); minBtn.setPadding(dp(ctx,10), dp(ctx,4), dp(ctx,10), dp(ctx,4));
        minBtn.setOnClickListener(v -> {
            mCollapsed.setVisibility(View.VISIBLE);
            mExpanded.setVisibility(View.GONE);
        });
        safe(minBtn);

        btnBar.addView(ver); btnBar.addView(hideBtn); btnBar.addView(minBtn);
        mExpanded.addView(btnBar);

        // ── Window params ─────────────────────────────────────────────────────
        menuParams = new WindowManager.LayoutParams(
            WRAP_CONTENT, WRAP_CONTENT, TYPE_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
            | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT);
        menuParams.gravity = Gravity.TOP | Gravity.START;
        menuParams.x = 0; menuParams.y = 80;

        rootFrame.setOnTouchListener(makeDrag());
        rootFrame.addView(mRootContainer);
        mRootContainer.addView(mCollapsed);
        mRootContainer.addView(mRestoreDot);
        mRootContainer.addView(mExpanded);
        try { wm.addView(rootFrame, menuParams); } catch (Exception e) {}

        // Build feature list setelah layout siap
        new Handler().postDelayed(() -> {
            patches.removeAllViews();
            buildFeatures(FEATURES, patches, ctx);
        }, 300);
    }

    // =========================================================================
    // FEATURE BUILDER
    // =========================================================================
    private void buildFeatures(String[] list, LinearLayout parent, Context ctx) {
        int sub = 0;
        for (int i = 0; i < list.length; i++) {
            String feat = list[i];
            String[] st = feat.split("_");
            int fn;
            if (TextUtils.isDigitsOnly(st[0]) || st[0].matches("-[0-9]*")) {
                fn = Integer.parseInt(st[0]);
                feat = feat.replaceFirst(st[0] + "_", "");
                sub++;
            } else { fn = i - sub; }
            String[] sp = feat.split("_");
            if      (sp[0].equals("Toggle"))   parent.addView(buildToggle(fn, sp[1], false, ctx));
            else if (sp[0].equals("SeekBar"))  parent.addView(buildSeekBar(fn, sp[1], Integer.parseInt(sp[2]), Integer.parseInt(sp[3]), ctx));
            else if (sp[0].equals("Spinner"))  { parent.addView(buildColorPicker(fn, sp[2], ctx)); }
            else if (sp[0].equals("Category")) { sub++; parent.addView(buildCategory(sp[1], ctx)); }
        }
    }

    private View buildToggle(final int fn, final String name, boolean def, Context ctx) {
        final boolean[] state = {def};
        final LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(dp(ctx,16), dp(ctx,9), dp(ctx,12), dp(ctx,9));
        row.setGravity(Gravity.CENTER_VERTICAL);

        final TextView label = new TextView(ctx); label.setText(name);
        label.setTextColor(C_TEXT_MAIN); label.setTextSize(13f);
        label.setLayoutParams(new LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f));

        final TextView pill = new TextView(ctx);
        pill.setTextSize(10f); pill.setTypeface(Typeface.DEFAULT_BOLD);
        pill.setPadding(dp(ctx,14), dp(ctx,4), dp(ctx,14), dp(ctx,4));
        pill.setGravity(Gravity.CENTER);

        final Runnable upd = () -> {
            if (state[0]) { pill.setText("ON");  pill.setTextColor(Color.WHITE); pill.setBackgroundColor(C_ON); }
            else          { pill.setText("OFF"); pill.setTextColor(C_TEXT_DIM);  pill.setBackgroundColor(C_OFF); }
        };
        upd.run();

        View.OnClickListener click = v -> { state[0] = !state[0]; upd.run(); applyToggle(fn, state[0]); };
        row.setOnClickListener(click); pill.setOnClickListener(click);
        safe(row); safe(pill);
        row.addView(label); row.addView(pill);
        return row;
    }

    private void applyToggle(int fn, boolean v) {
        switch (fn) {
            case 1: OverlayESP.espLine     = v; if(mShm!=null) mShm.setEspLine(v);     break;
            case 2: OverlayESP.espBox      = v; if(mShm!=null) mShm.setEspBox(v);      break;
            case 3: OverlayESP.espHealth   = v; if(mShm!=null) mShm.setEspHealth(v);   break;
            case 4: OverlayESP.espName     = v; if(mShm!=null) mShm.setEspName(v);     break;
            case 5: OverlayESP.espDistance = v; if(mShm!=null) mShm.setEspDistance(v); break;
            case 6:                             if(mShm!=null) mShm.setShowFPS(v);     break;
        }
    }

    private View buildSeekBar(final int fn, final String name, final int min, int max, Context ctx) {
        LinearLayout ll = new LinearLayout(ctx);
        ll.setPadding(dp(ctx,16), dp(ctx,8), dp(ctx,16), dp(ctx,6));
        ll.setOrientation(LinearLayout.VERTICAL);
        final TextView tv = new TextView(ctx);
        tv.setText(Html.fromHtml(name + ": <font color='#1A73E8'>" + min + "</font>"));
        tv.setTextColor(C_TEXT_MAIN); tv.setTextSize(12f);
        SeekBar sb = new SeekBar(ctx);
        sb.setPadding(0, dp(ctx,6), 0, dp(ctx,4));
        sb.setMax(max);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) sb.setMin(min);
        sb.setProgress(min);
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onStartTrackingTouch(SeekBar s) {}
            public void onStopTrackingTouch(SeekBar s) {}
            public void onProgressChanged(SeekBar s, int val, boolean z) {
                int v2 = Math.max(val, min);
                tv.setText(Html.fromHtml(name + ": <font color='#1A73E8'>" + v2 + "</font>"));
                if (fn == -7 && mShm != null) mShm.setFov(v2);
            }
        });
        ll.addView(tv); ll.addView(sb);
        return ll;
    }

    private View buildColorPicker(final int fn, final String list, Context ctx) {
        final String[] items = list.split(",");
        final int[] COLS = {
            Color.rgb(255,50,50), Color.WHITE, Color.rgb(30,110,230),
            Color.rgb(230,180,0), Color.rgb(0,190,200), Color.rgb(50,220,50)
        };
        LinearLayout ll = new LinearLayout(ctx);
        ll.setOrientation(LinearLayout.HORIZONTAL);
        ll.setPadding(dp(ctx,16), dp(ctx,8), dp(ctx,16), dp(ctx,8));
        ll.setGravity(Gravity.CENTER_VERTICAL);
        final TextView[] btns = new TextView[items.length];
        for (int idx = 0; idx < items.length; idx++) {
            final int pos = idx;
            final int dotC = pos < COLS.length ? COLS[pos] : Color.WHITE;
            final TextView dot = new TextView(ctx);
            int sz = dp(ctx, 24);
            LinearLayout.LayoutParams lp2 = new LinearLayout.LayoutParams(sz, sz);
            lp2.setMargins(dp(ctx,2), 0, dp(ctx,2), 0);
            dot.setLayoutParams(lp2);
            dot.setBackgroundColor(dotC); dot.setGravity(Gravity.CENTER);
            dot.setText(pos == OverlayESP.colorIndex ? "✓" : "");
            dot.setTextColor(dotC == Color.WHITE ? Color.BLACK : Color.WHITE);
            dot.setTextSize(9f);
            safe(dot);
            dot.setOnClickListener(v -> {
                for (TextView b : btns) if (b != null) b.setText("");
                dot.setText("✓");
                OverlayESP.colorIndex = pos;
                if (mShm != null) mShm.setColorIndex(pos);
            });
            btns[idx] = dot;
            ll.addView(dot);
        }
        return ll;
    }

    private View buildCategory(String text, Context ctx) {
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setBackgroundColor(C_BG_CATEGORY);
        row.setPadding(0, dp(ctx,8), dp(ctx,16), dp(ctx,8));
        View bar = new View(ctx);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(dp(ctx,3), dp(ctx,13));
        blp.setMargins(dp(ctx,12), 0, dp(ctx,8), 0);
        bar.setLayoutParams(blp); bar.setBackgroundColor(C_ACCENT);
        TextView tv = new TextView(ctx); tv.setText(text);
        tv.setTextColor(C_TEXT_ACCENT); tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setTextSize(10f); tv.setAllCaps(true);
        row.addView(bar); row.addView(tv);
        return row;
    }

    private View divider(Context ctx) {
        View v = new View(ctx);
        v.setLayoutParams(new LinearLayout.LayoutParams(MATCH_PARENT, 1));
        v.setBackgroundColor(C_DIVIDER);
        return v;
    }

    // =========================================================================
    // DRAG
    // =========================================================================
    private View.OnTouchListener makeDrag() {
        return new View.OnTouchListener() {
            float iTX, iTY; int iX, iY;
            public boolean onTouch(View v, MotionEvent e) {
                switch (e.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        iX = menuParams.x; iY = menuParams.y;
                        iTX = e.getRawX(); iTY = e.getRawY();
                        return true;
                    case MotionEvent.ACTION_UP:
                        int dx = (int)(e.getRawX()-iTX), dy = (int)(e.getRawY()-iTY);
                        mExpanded.setAlpha(1f); mCollapsed.setAlpha(1f);
                        if (Math.abs(dx)<10 && Math.abs(dy)<10
                                && mCollapsed.getVisibility()==View.VISIBLE) {
                            mCollapsed.setVisibility(View.GONE);
                            mExpanded.setVisibility(View.VISIBLE);
                        }
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        mExpanded.setAlpha(0.7f); mCollapsed.setAlpha(0.7f);
                        menuParams.x = iX + (int)(e.getRawX()-iTX);
                        menuParams.y = iY + (int)(e.getRawY()-iTY);
                        try { wm.updateViewLayout(rootFrame, menuParams); } catch (Exception ex) {}
                        return true;
                }
                return false;
            }
        };
    }

    private int dp(Context ctx, int i) {
        return (int) TypedValue.applyDimension(1, i, ctx.getResources().getDisplayMetrics());
    }
}
