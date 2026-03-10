package uk.lgl;

// =============================================================================
// OverlayMain.java — Modified untuk Hybrid ImGui Architecture
//
// PERUBAHAN UTAMA DARI VERSI ASLI:
//   1. Ditambahkan ImguiSurfaceView: SurfaceView fullscreen transparan
//      yang menjadi "canvas" untuk ImGui render dari C++
//   2. Ditambahkan JNI bridge: nativeInitImGui, nativeInjectTouch, dll
//   3. Ditambahkan WantCapture loop: update FLAG_NOT_TOUCHABLE setiap 100ms
//      agar touch pass-through ke game saat menu ImGui tidak terlihat
//   4. OverlayESP + SHM tetap ada (untuk ESP drawing yang tetap pakai Canvas)
//
// ARSITEKTUR WINDOW STACK (dari bawah ke atas):
//   [Game Surface]
//   [OverlayESP Window]   ← Canvas ESP, FLAG_NOT_TOUCHABLE
//   [ImGui Surface Window] ← SurfaceView, touchable saat ImGui aktif
//   [Java Menu Window]    ← Floating icon (minimal, untuk debug/fallback)
// =============================================================================

import android.content.Context;
import android.content.ContextWrapper;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;

import java.lang.reflect.Method;

public class OverlayMain {

    static final String SHM_PATH = "/data/data/com.mobiin.gp/files/mlbb_esp.shm";
    static final int TYPE_OVERLAY = 2038; // TYPE_APPLICATION_OVERLAY fallback

    // =========================================================================
    // JNI BRIDGE — Native methods dari libimgui_ext.so
    // =========================================================================
    static {
        try {
            System.loadLibrary("imgui_ext");
        } catch (UnsatisfiedLinkError e) {
            System.err.println("[OverlayMain] Failed to load libimgui_ext.so: " + e.getMessage());
        }
    }

    /**
     * Inisialisasi ImGui renderer C++.
     * Dipanggil saat SurfaceHolder.Callback.surfaceCreated() terpanggil.
     * C++ akan: ambil ANativeWindow → init EGL → init ImGui → start render thread.
     *
     * @param surface  java.lang.Surface dari SurfaceHolder
     * @param w        Lebar awal surface
     * @param h        Tinggi awal surface
     */
    private static native void nativeInitImGui(Surface surface, int w, int h);

    /**
     * Forward touch event dari Java ke ImGui (thread-safe queue).
     * Dipanggil dari dispatchTouchEvent() SurfaceView.
     *
     * @param x      Koordinat X sentuhan (pixel)
     * @param y      Koordinat Y sentuhan (pixel)
     * @param action 0=DOWN, 1=UP, 2=MOVE (MotionEvent.ACTION_*)
     */
    private static native void nativeInjectTouch(float x, float y, int action);

    /**
     * Notify C++ bahwa surface berubah ukuran.
     * Dipanggil dari SurfaceHolder.Callback.surfaceChanged().
     */
    private static native void nativeOnSurfaceChanged(Surface surface, int w, int h);

    /**
     * Destroy C++ ImGui renderer (stop render thread, release EGL).
     * Dipanggil dari SurfaceHolder.Callback.surfaceDestroyed().
     */
    private static native void nativeDestroy();

    /**
     * @return true jika ImGui sedang "ingin" menangkap input
     *         (ada window ImGui yang di-hover atau di-drag oleh user)
     */
    private static native boolean nativeWantsCapture();

    /**
     * @return true jika render thread C++ sedang berjalan
     */
    private static native boolean nativeIsRunning();

    // =========================================================================
    // ENTRY POINT
    // =========================================================================
    public static void main(String[] args) {
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread t, Throwable e) {
                System.err.println("[OverlayMain] CRASH: " + e.getMessage());
                e.printStackTrace();
            }
        });
        Looper.prepareMainLooper();

        // OOM score adjustment — cegah proses di-kill
        try {
            int pid = android.os.Process.myPid();
            Runtime.getRuntime().exec(new String[]{
                "su", "-c",
                "echo -1000 > /proc/" + pid + "/oom_score_adj"
            });
        } catch (Exception e) { /* ignore */ }

        final Context ctx = getContextReflection();
        if (ctx == null) {
            System.err.println("[OverlayMain] getContext() failed!");
            return;
        }

        // Tunggu SHM dari libinternal.so siap (tetap ada untuk ESP)
        final SHMBridge shm = new SHMBridge();
        final Handler mainHandler = new Handler(Looper.getMainLooper());

        Thread shmThread = new Thread(new Runnable() {
            @Override
            public void run() {
                // Coba connect ke SHM file (dibuat oleh libinternal.so)
                boolean connected = false;
                for (int i = 0; i < 120 && !connected; i++) {
                    connected = shm.connect();
                    if (!connected) {
                        try { Thread.sleep(1000); } catch (Exception e2) { /* */ }
                    }
                }
                // Apakah SHM konek atau tidak, tetap launch overlay
                // ImGui bisa berjalan tanpa SHM (SHM hanya untuk ESP Canvas)
                final boolean shmOk = connected;
                mainHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        new OverlayMain().startOverlay(ctx, shmOk ? shm : null);
                    }
                });
            }
        });
        shmThread.setDaemon(false);
        shmThread.start();

        Looper.loop();
    }

    // =========================================================================
    // Reflection untuk dapatkan Context di app_process
    // =========================================================================
    private static Context getContextReflection() {
        try {
            Class<?> c = Class.forName("android.app.ActivityThread");
            Method sm = c.getDeclaredMethod("systemMain");
            sm.setAccessible(true);
            Object at = sm.invoke(null);
            Method gs = c.getDeclaredMethod("getSystemContext");
            gs.setAccessible(true);
            return (Context) gs.invoke(at);
        } catch (Exception e) {
            System.err.println("[OverlayMain] getContextReflection: " + e.getMessage());
        }
        return null;
    }

    // =========================================================================
    // SAFE CONTEXT WRAPPER
    // =========================================================================
    private static Context makeSafeCtx(final Context base) {
        return new ContextWrapper(base) {
            @Override
            public Object getSystemService(String name) {
                if (AUDIO_SERVICE.equals(name))         return null;
                if (ACCESSIBILITY_SERVICE.equals(name)) return null;
                if (VIBRATOR_SERVICE.equals(name))      return null;
                try { return super.getSystemService(name); }
                catch (Exception e) { return null; }
            }
        };
    }

    // =========================================================================
    // STATE
    // =========================================================================
    private WindowManager          wm;
    private OverlayESP             espView;
    private WindowManager.LayoutParams espParams;

    // ImGui Surface Window
    private ImguiSurfaceView        imguiSurfaceView;
    private WindowManager.LayoutParams imguiParams;

    // Handler untuk WantCapture polling
    private Handler                 captureHandler;
    private Runnable                captureChecker;

    // =========================================================================
    // START OVERLAY
    // =========================================================================
    private void startOverlay(final Context ctx, final SHMBridge shm) {
        Context safeCtx = makeSafeCtx(ctx);
        wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);

        // ── 1. ESP Overlay (Canvas, tetap dari arsitektur lama) ────────────
        if (shm != null) {
            espView   = new OverlayESP(ctx, shm);
            espParams = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                TYPE_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
            espParams.gravity = Gravity.TOP | Gravity.START;
            if (Build.VERSION.SDK_INT >= 28) espParams.layoutInDisplayCutoutMode = 1;
            try { wm.addView(espView, espParams); } catch (Exception e) { /* */ }
        }

        // ── 2. ImGui SurfaceView Window ─────────────────────────────────────
        imguiSurfaceView = new ImguiSurfaceView(safeCtx);
        imguiParams = new WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            TYPE_OVERLAY,
            // Awalnya NOT_TOUCHABLE — akan diubah dinamis saat ImGui aktif
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
            | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE, // OFF saat ImGui aktif
            PixelFormat.TRANSLUCENT);
        imguiParams.gravity = Gravity.TOP | Gravity.START;
        if (Build.VERSION.SDK_INT >= 28) imguiParams.layoutInDisplayCutoutMode = 1;

        try { wm.addView(imguiSurfaceView, imguiParams); }
        catch (Exception e) {
            System.err.println("[OverlayMain] addView ImGui failed: " + e.getMessage());
        }

        // ── 3. WantCapture polling: update touchable flag setiap 100ms ──────
        captureHandler  = new Handler(Looper.getMainLooper());
        captureChecker  = new Runnable() {
            private boolean lastCapture = false;

            @Override
            public void run() {
                if (!nativeIsRunning()) {
                    // Render tidak berjalan, jadikan non-touchable
                    setImguiTouchable(false);
                } else {
                    boolean wants = nativeWantsCapture();
                    if (wants != lastCapture) {
                        setImguiTouchable(wants);
                        lastCapture = wants;
                    }
                }
                captureHandler.postDelayed(this, 100);
            }
        };
        captureHandler.postDelayed(captureChecker, 200);

        // ── 4. Revive loop: pastikan view tidak hilang ───────────────────────
        final Handler revive = new Handler(Looper.getMainLooper());
        revive.post(new Runnable() {
            @Override
            public void run() {
                try {
                    if (espView != null && !espView.isAttachedToWindow() && shm != null) {
                        wm.addView(espView, espParams);
                    }
                } catch (Exception e) { /* */ }
                try {
                    if (imguiSurfaceView != null && !imguiSurfaceView.isAttachedToWindow()) {
                        wm.addView(imguiSurfaceView, imguiParams);
                    }
                } catch (Exception e) { /* */ }
                revive.postDelayed(this, 2000);
            }
        });
    }

    // =========================================================================
    // Helper: update touchable flag WindowManager
    // =========================================================================
    private void setImguiTouchable(boolean touchable) {
        if (imguiParams == null || wm == null) return;
        if (touchable) {
            imguiParams.flags &= ~WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        } else {
            imguiParams.flags |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        }
        try {
            if (imguiSurfaceView != null && imguiSurfaceView.isAttachedToWindow()) {
                wm.updateViewLayout(imguiSurfaceView, imguiParams);
            }
        } catch (Exception e) { /* */ }
    }

    // =========================================================================
    // ImguiSurfaceView — Inner class SurfaceView untuk ImGui
    //
    // Mengimplementasikan SurfaceHolder.Callback untuk lifecycle management:
    //   surfaceCreated  → nativeInitImGui()
    //   surfaceChanged  → nativeOnSurfaceChanged()
    //   surfaceDestroyed → nativeDestroy()
    //
    // Override dispatchTouchEvent untuk forward touch ke ImGui C++.
    // =========================================================================
    private static class ImguiSurfaceView extends SurfaceView
        implements SurfaceHolder.Callback {

        public ImguiSurfaceView(Context ctx) {
            super(ctx);

            // PENTING: setFormat ke TRANSLUCENT agar background transparan
            // Ini yang memungkinkan kita "melihat" game di balik ImGui window
            getHolder().setFormat(PixelFormat.TRANSLUCENT);
            getHolder().addCallback(this);

            // Disable fitur Android yang bisa mengganggu
            setSoundEffectsEnabled(false);
            setHapticFeedbackEnabled(false);
            setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);

            // Z-order: pastikan di atas ESP canvas
            setZOrderMediaOverlay(true);

            System.out.println("[ImguiSurfaceView] Created");
        }

        // ── SurfaceHolder.Callback ─────────────────────────────────────────

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            System.out.println("[ImguiSurfaceView] surfaceCreated");

            // Dapatkan dimensi display
            android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
            // getDisplay() tersedia API 17+
            if (Build.VERSION.SDK_INT >= 17) {
                try {
                    Class<?> wm = Class.forName("android.view.WindowManager");
                    // Gunakan getWindowVisibleDisplayFrame sebagai fallback
                } catch (Exception e) { /* */ }
            }

            // Gunakan dimensi surface langsung
            int w = holder.getSurfaceFrame().width();
            int h = holder.getSurfaceFrame().height();

            if (w == 0 || h == 0) {
                // Fallback ke display metrics default
                w = getResources().getDisplayMetrics().widthPixels;
                h = getResources().getDisplayMetrics().heightPixels;
            }

            System.out.println("[ImguiSurfaceView] surfaceCreated " + w + "x" + h);

            // Panggil JNI bridge
            nativeInitImGui(holder.getSurface(), w, h);
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            System.out.println("[ImguiSurfaceView] surfaceChanged " + width + "x" + height);
            nativeOnSurfaceChanged(holder.getSurface(), width, height);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            System.out.println("[ImguiSurfaceView] surfaceDestroyed");
            nativeDestroy();
        }

        // ── Touch forwarding ───────────────────────────────────────────────
        //
        // Override dispatchTouchEvent (bukan onTouchEvent) agar kita bisa
        // memproses touch SEBELUM View hierarchy normal memprosesnya.
        // Ini memastikan tidak ada touch yang "hilang" di tengah jalan.
        //
        @Override
        public boolean dispatchTouchEvent(MotionEvent event) {
            // Ambil koordinat dan action dari event
            float x      = event.getX();
            float y      = event.getY();
            int   action = event.getActionMasked();

            // Forward ke ImGui C++ (thread-safe queue di C++)
            // Hanya forward action yang kita kenal (DOWN=0, UP=1, MOVE=2)
            if (action == MotionEvent.ACTION_DOWN ||
                action == MotionEvent.ACTION_UP   ||
                action == MotionEvent.ACTION_MOVE) {
                nativeInjectTouch(x, y, action);
            }

            // Juga handle ACTION_CANCEL sebagai ACTION_UP
            if (action == MotionEvent.ACTION_CANCEL) {
                nativeInjectTouch(x, y, 1); // Treat as UP
            }

            // Kembalikan true hanya jika ImGui mau handle touch ini
            // ImGui mengeset WantCaptureMouse satu frame setelah touch terjadi,
            // jadi kita harus return true untuk semua touch agar tidak terlewat.
            // Java handler akan mengatur FLAG_NOT_TOUCHABLE untuk pass-through.
            return true;
        }
    }
}
