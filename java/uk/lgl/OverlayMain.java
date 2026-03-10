package uk.lgl;

import android.content.Context;
import android.content.ContextWrapper;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
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

import java.io.RandomAccessFile;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;

public class OverlayMain {

    static final String SHM_PATH   = "/data/data/com.mobiin.gp/files/mlbb_esp.shm";
    static final int    TYPE_OVERLAY = 2038;

    // =========================================================================
    // INNER CLASS: SHMBridge
    // =========================================================================
    public static class SHMBridge {
        private static final int MAGIC    = 0xDEADBEEF;
        private static final int SHM_SIZE = 4096;

        public static class EntityData {
            public float x, y, hp, maxHp, dist;
            public int   team, type;
            public boolean valid;
        }

        private MappedByteBuffer mMap;
        private boolean mConnected = false;

        public boolean connect() {
            try {
                java.io.File f = new java.io.File(SHM_PATH);
                if (!f.exists()) return false;
                RandomAccessFile raf = new RandomAccessFile(f, "r");
                mMap = raf.getChannel().map(FileChannel.MapMode.READ_ONLY, 0, SHM_SIZE);
                mMap.order(ByteOrder.LITTLE_ENDIAN);
                int magic = mMap.getInt(0);
                raf.close();
                if (magic != MAGIC) { mMap = null; return false; }
                mConnected = true;
                return true;
            } catch (Exception e) { mConnected = false; return false; }
        }

        public boolean isConnected() { return mConnected && mMap != null; }

        public EntityData[] readEntities() {
            if (!isConnected()) return new EntityData[0];
            try {
                int count = mMap.getInt(8);
                if (count <= 0 || count > 64) return new EntityData[0];
                EntityData[] result = new EntityData[count];
                for (int i = 0; i < count; i++) {
                    int off = 12 + i * 64;
                    EntityData e = new EntityData();
                    e.x     = mMap.getFloat(off);
                    e.y     = mMap.getFloat(off + 4);
                    e.hp    = mMap.getFloat(off + 8);
                    e.maxHp = mMap.getFloat(off + 12);
                    e.team  = mMap.getInt  (off + 16);
                    e.type  = mMap.getInt  (off + 20);
                    e.dist  = mMap.getFloat(off + 24);
                    e.valid = mMap.getInt  (off + 28) == 1;
                    result[i] = e;
                }
                return result;
            } catch (Exception e) { return new EntityData[0]; }
        }
    }

    // =========================================================================
    // INNER CLASS: OverlayESP
    // =========================================================================
    public static class OverlayESP extends View {
        private static final long REFRESH_MS = 50;

        private final Paint paintEnemy, paintAlly, paintText, paintHP, paintHPBg;
        private final SHMBridge shm;
        private SHMBridge.EntityData[] entities = new SHMBridge.EntityData[0];
        private final Handler  handler   = new Handler(Looper.getMainLooper());
        private final Runnable refresher = this::tick;

        public OverlayESP(Context ctx, SHMBridge shm) {
            super(ctx);
            this.shm = shm;

            paintEnemy = new Paint(Paint.ANTI_ALIAS_FLAG);
            paintEnemy.setColor(Color.rgb(255, 50, 50));
            paintEnemy.setStyle(Paint.Style.STROKE);
            paintEnemy.setStrokeWidth(2f);

            paintAlly = new Paint(Paint.ANTI_ALIAS_FLAG);
            paintAlly.setColor(Color.rgb(50, 255, 100));
            paintAlly.setStyle(Paint.Style.STROKE);
            paintAlly.setStrokeWidth(2f);

            paintText = new Paint(Paint.ANTI_ALIAS_FLAG);
            paintText.setColor(Color.WHITE);
            paintText.setTextSize(28f);
            paintText.setTypeface(Typeface.MONOSPACE);
            paintText.setShadowLayer(3f, 1f, 1f, Color.BLACK);

            paintHPBg = new Paint();
            paintHPBg.setColor(Color.argb(180, 0, 0, 0));
            paintHPBg.setStyle(Paint.Style.FILL);

            paintHP = new Paint();
            paintHP.setStyle(Paint.Style.FILL);

            setWillNotDraw(false);
            handler.postDelayed(refresher, REFRESH_MS);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (entities == null) return;
            for (SHMBridge.EntityData e : entities) {
                if (e == null || !e.valid) continue;
                float sx = e.x, sy = e.y;
                if (sx < 0 || sx > getWidth() || sy < 0 || sy > getHeight()) continue;
                Paint box = (e.team == 0) ? paintEnemy : paintAlly;
                float h = Math.max(20f, 200f - e.dist * 0.5f);
                float w = h * 0.5f;
                canvas.drawRect(sx - w/2, sy - h, sx + w/2, sy, box);
                float ratio = (e.maxHp > 0) ? Math.min(1f, e.hp / e.maxHp) : 0f;
                canvas.drawRect(sx - w/2, sy + 4, sx + w/2, sy + 10, paintHPBg);
                paintHP.setColor(ratio > 0.6f ? Color.rgb(50,220,50) :
                                 ratio > 0.3f ? Color.rgb(255,200,0) : Color.rgb(255,60,60));
                canvas.drawRect(sx - w/2, sy + 4, sx - w/2 + w * ratio, sy + 10, paintHP);
                canvas.drawText(String.format("%.0fm", e.dist), sx - w/2, sy - h - 6, paintText);
            }
        }

        private void tick() {
            if (!isAttachedToWindow()) return;
            if (shm != null && shm.isConnected()) entities = shm.readEntities();
            invalidate();
            handler.postDelayed(refresher, REFRESH_MS);
        }

        @Override
        protected void onDetachedFromWindow() {
            super.onDetachedFromWindow();
            handler.removeCallbacks(refresher);
        }
    }

    // =========================================================================
    // JNI BRIDGE
    // =========================================================================
    static {
        try { System.loadLibrary("imgui_ext"); }
        catch (UnsatisfiedLinkError e) {
            System.err.println("[OverlayMain] Failed to load libimgui_ext.so: " + e.getMessage());
        }
    }

    private static native void nativeInitImGui(Surface surface, int w, int h);
    private static native void nativeInjectTouch(float x, float y, int action);
    private static native void nativeOnSurfaceChanged(Surface surface, int w, int h);
    private static native void nativeDestroy();
    private static native boolean nativeWantsCapture();
    private static native boolean nativeIsRunning();

    // =========================================================================
    // ENTRY POINT
    // =========================================================================
    public static void main(String[] args) {
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            System.err.println("[OverlayMain] CRASH: " + e.getMessage());
            e.printStackTrace();
        });
        Looper.prepareMainLooper();

        try {
            int pid = android.os.Process.myPid();
            Runtime.getRuntime().exec(new String[]{"su", "-c",
                "echo -1000 > /proc/" + pid + "/oom_score_adj"});
        } catch (Exception e) { /* ignore */ }

        final Context ctx = getContextReflection();
        if (ctx == null) {
            System.err.println("[OverlayMain] getContext() failed!");
            return;
        }

        final SHMBridge shm = new SHMBridge();
        final Handler mainHandler = new Handler(Looper.getMainLooper());

        new Thread(() -> {
            boolean connected = false;
            for (int i = 0; i < 120 && !connected; i++) {
                connected = shm.connect();
                if (!connected) {
                    try { Thread.sleep(1000); } catch (Exception e2) { /* */ }
                }
            }
            final boolean shmOk = connected;
            mainHandler.post(() -> new OverlayMain().startOverlay(ctx, shmOk ? shm : null));
        }).start();

        Looper.loop();
    }

    private static Context getContextReflection() {
        try {
            Class<?> c  = Class.forName("android.app.ActivityThread");
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
    private WindowManager wm;
    private OverlayESP    espView;
    private WindowManager.LayoutParams espParams;
    private ImguiSurfaceView imguiSurfaceView;
    private WindowManager.LayoutParams imguiParams;
    private Handler captureHandler;
    private Runnable captureChecker;

    // =========================================================================
    // START OVERLAY
    // =========================================================================
    private void startOverlay(final Context ctx, final SHMBridge shm) {
        Context safeCtx = makeSafeCtx(ctx);
        wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);

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

        imguiSurfaceView = new ImguiSurfaceView(safeCtx);
        imguiParams = new WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            TYPE_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
            | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE,
            PixelFormat.TRANSLUCENT);
        imguiParams.gravity = Gravity.TOP | Gravity.START;
        if (Build.VERSION.SDK_INT >= 28) imguiParams.layoutInDisplayCutoutMode = 1;
        try { wm.addView(imguiSurfaceView, imguiParams); }
        catch (Exception e) { System.err.println("[OverlayMain] addView ImGui failed: " + e); }

        captureHandler = new Handler(Looper.getMainLooper());
        captureChecker = new Runnable() {
            private boolean lastCapture = false;
            @Override public void run() {
                if (!nativeIsRunning()) {
                    setImguiTouchable(false);
                } else {
                    boolean wants = nativeWantsCapture();
                    if (wants != lastCapture) { setImguiTouchable(wants); lastCapture = wants; }
                }
                captureHandler.postDelayed(this, 100);
            }
        };
        captureHandler.postDelayed(captureChecker, 200);

        final Handler revive = new Handler(Looper.getMainLooper());
        revive.post(new Runnable() {
            @Override public void run() {
                try {
                    if (espView != null && !espView.isAttachedToWindow() && shm != null)
                        wm.addView(espView, espParams);
                } catch (Exception e) { /* */ }
                try {
                    if (imguiSurfaceView != null && !imguiSurfaceView.isAttachedToWindow())
                        wm.addView(imguiSurfaceView, imguiParams);
                } catch (Exception e) { /* */ }
                revive.postDelayed(this, 2000);
            }
        });
    }

    private void setImguiTouchable(boolean touchable) {
        if (imguiParams == null || wm == null) return;
        if (touchable) imguiParams.flags &= ~WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        else           imguiParams.flags |=  WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        try {
            if (imguiSurfaceView != null && imguiSurfaceView.isAttachedToWindow())
                wm.updateViewLayout(imguiSurfaceView, imguiParams);
        } catch (Exception e) { /* */ }
    }

    // =========================================================================
    // ImguiSurfaceView
    // =========================================================================
    private static class ImguiSurfaceView extends SurfaceView implements SurfaceHolder.Callback {

        public ImguiSurfaceView(Context ctx) {
            super(ctx);
            getHolder().setFormat(PixelFormat.TRANSLUCENT);
            getHolder().addCallback(this);
            setSoundEffectsEnabled(false);
            setHapticFeedbackEnabled(false);
            setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
            setZOrderMediaOverlay(true);
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            int w = holder.getSurfaceFrame().width();
            int h = holder.getSurfaceFrame().height();
            if (w == 0 || h == 0) {
                w = getResources().getDisplayMetrics().widthPixels;
                h = getResources().getDisplayMetrics().heightPixels;
            }
            nativeInitImGui(holder.getSurface(), w, h);
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
            nativeOnSurfaceChanged(holder.getSurface(), w, h);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) { nativeDestroy(); }

        @Override
        public boolean dispatchTouchEvent(MotionEvent event) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN ||
                action == MotionEvent.ACTION_UP   ||
                action == MotionEvent.ACTION_MOVE) {
                nativeInjectTouch(event.getX(), event.getY(), action);
            }
            if (action == MotionEvent.ACTION_CANCEL) nativeInjectTouch(event.getX(), event.getY(), 1);
            return true;
        }
    }
}
