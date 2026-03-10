package uk.lgl;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.view.View;

/**
 * OverlayESP — Canvas fullscreen transparan untuk menggambar ESP.
 *
 * Data dibaca dari SHMBridge (shared memory file yang ditulis libinternal.so).
 * Flow:
 *   libinternal.so (game process) → scan IL2CPP objects → tulis ke SHM
 *   OverlayESP (overlay process)  → baca SHM → gambar via Canvas
 *
 * Overlay tidak perlu tunggu library dimuat.
 * Cukup poll SHM sampai ready=true, lalu mulai gambar.
 */
public class OverlayESP extends View {

    private static final long REFRESH_MS = 50; // 20 FPS

    // ── Paint objects ──────────────────────────────────────────────────────────
    private final Paint paintBox;
    private final Paint paintText;
    private final Paint paintHP;
    private final Paint paintHPBg;
    private final Paint paintLine;
    private final Paint paintDot;

    // ── State ──────────────────────────────────────────────────────────────────
    private final SHMBridge shm;
    private final Handler   handler   = new Handler(Looper.getMainLooper());
    private final Runnable  refresher = this::refreshAndDraw;

    // Snapshot data (diupdate tiap refresh)
    private int     entityCount = 0;
    private float[] screenX     = new float[SHMBridge.MAX_ENTITIES];
    private float[] screenY     = new float[SHMBridge.MAX_ENTITIES];
    private float[] headX       = new float[SHMBridge.MAX_ENTITIES];
    private float[] headY       = new float[SHMBridge.MAX_ENTITIES];
    private int[]   hp          = new int[SHMBridge.MAX_ENTITIES];
    private int[]   hpMax       = new int[SHMBridge.MAX_ENTITIES];
    private float[] dist        = new float[SHMBridge.MAX_ENTITIES];
    private boolean[] valid     = new boolean[SHMBridge.MAX_ENTITIES];
    private String[]  names     = new String[SHMBridge.MAX_ENTITIES];

    // Config flags (diupdate dari OverlayMain menu)
    public static volatile boolean espLine     = true;
    public static volatile boolean espBox      = true;
    public static volatile boolean espHealth   = true;
    public static volatile boolean espName     = false;
    public static volatile boolean espDistance = true;
    public static volatile int     colorIndex  = 0; // 0=Red,1=White,2=Blue,3=Yellow,4=Cyan,5=Green

    // ── Colors ─────────────────────────────────────────────────────────────────
    private static final int[] COLORS = {
        Color.rgb(255, 50, 50),    // 0 Red
        Color.WHITE,               // 1 White
        Color.rgb(30, 110, 230),   // 2 Blue
        Color.rgb(230, 180, 0),    // 3 Yellow
        Color.rgb(0, 190, 200),    // 4 Cyan
        Color.rgb(50, 220, 50),    // 5 Green
    };

    // =========================================================================
    public OverlayESP(Context ctx, SHMBridge shm) {
        super(ctx);
        this.shm = shm;

        paintBox = new Paint(Paint.ANTI_ALIAS_FLAG);
        paintBox.setStyle(Paint.Style.STROKE);
        paintBox.setStrokeWidth(2f);

        paintText = new Paint(Paint.ANTI_ALIAS_FLAG);
        paintText.setColor(Color.WHITE);
        paintText.setTextSize(26f);
        paintText.setTypeface(Typeface.MONOSPACE);
        paintText.setShadowLayer(3f, 1f, 1f, Color.BLACK);

        paintHPBg = new Paint();
        paintHPBg.setColor(Color.argb(180, 0, 0, 0));
        paintHPBg.setStyle(Paint.Style.FILL);

        paintHP = new Paint();
        paintHP.setStyle(Paint.Style.FILL);

        paintLine = new Paint(Paint.ANTI_ALIAS_FLAG);
        paintLine.setStrokeWidth(1.2f);

        paintDot = new Paint(Paint.ANTI_ALIAS_FLAG);
        paintDot.setStyle(Paint.Style.FILL);

        setWillNotDraw(false);
        handler.postDelayed(refresher, REFRESH_MS);
    }

    // =========================================================================
    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (entityCount == 0) return;

        int color = COLORS[Math.min(colorIndex, COLORS.length - 1)];
        paintBox.setColor(color);
        paintLine.setColor(Color.argb(130, Color.red(color), Color.green(color), Color.blue(color)));
        paintDot.setColor(color);

        float cx = getWidth()  / 2f;
        float cy = getHeight() * 0.95f; // titik asal line ESP (bawah layar)

        for (int i = 0; i < entityCount; i++) {
            if (!valid[i]) continue;

            float sx = screenX[i];
            float sy = screenY[i];
            float hx = headX[i];
            float hy = headY[i];
            boolean onScreen = (sx >= 0 && sx <= getWidth() && sy >= 0 && sy <= getHeight());

            if (!onScreen) {
                // Edge indicator
                drawEdgeArrow(canvas, sx, sy, cx, cy, color);
                continue;
            }

            // ── Line from bottom center to entity ────────────────────────────
            if (espLine) {
                canvas.drawLine(cx, cy, sx, sy, paintLine);
            }

            // ── Bounding box (dari head ke kaki) ─────────────────────────────
            if (espBox && hy < sy) {
                float boxH = Math.abs(sy - hy);
                float boxW = boxH * 0.45f;
                canvas.drawRect(sx - boxW/2, hy, sx + boxW/2, sy, paintBox);

                // HP bar di dalam box (kanan)
                if (espHealth && hpMax[i] > 0) {
                    float ratio = Math.min(1f, (float)hp[i] / hpMax[i]);
                    float barX = sx + boxW/2 + 3;
                    float barH = boxH;
                    canvas.drawRect(barX, hy, barX + 6, hy + barH, paintHPBg);
                    paintHP.setColor(hpColor(ratio));
                    canvas.drawRect(barX, hy + barH*(1-ratio), barX + 6, hy + barH, paintHP);
                }
            } else {
                // Tanpa box: gambar dot
                canvas.drawCircle(sx, sy, 5f, paintDot);
            }

            // ── Text: nama + jarak ────────────────────────────────────────────
            float textY = (hy < sy) ? hy - 4 : sy - 20;
            if (espName && names[i] != null && !names[i].isEmpty()) {
                canvas.drawText(names[i], sx - paintText.measureText(names[i])/2, textY, paintText);
                textY -= 22;
            }
            if (espDistance) {
                String d = String.format("%.0fm", dist[i]);
                canvas.drawText(d, sx - paintText.measureText(d)/2, textY, paintText);
            }
        }
    }

    private void drawEdgeArrow(Canvas canvas, float tx, float ty, float cx, float cy, int color) {
        float angle = (float) Math.atan2(ty - cy, tx - cx);
        float ex = cx + (float) Math.cos(angle) * (getWidth()  / 2f - 24);
        float ey = cy + (float) Math.sin(angle) * (getHeight() / 2f - 24);
        ex = Math.max(12, Math.min(getWidth() - 12, ex));
        ey = Math.max(12, Math.min(getHeight() - 12, ey));
        paintDot.setColor(color);
        canvas.drawCircle(ex, ey, 7f, paintDot);
    }

    private int hpColor(float ratio) {
        if (ratio > 0.6f) return Color.rgb(50, 220, 50);
        if (ratio > 0.3f) return Color.rgb(255, 200, 0);
        return Color.rgb(255, 60, 60);
    }

    // =========================================================================
    private void refreshAndDraw() {
        if (!isAttachedToWindow()) return;

        if (shm != null && shm.isConnected() && shm.refresh()) {
            entityCount = shm.getEntityCount();
            int c = Math.min(entityCount, SHMBridge.MAX_ENTITIES);
            for (int i = 0; i < c; i++) {
                screenX[i] = shm.getEntityScreenX(i);
                screenY[i] = shm.getEntityScreenY(i);
                headX[i]   = shm.getEntityHeadX(i);
                headY[i]   = shm.getEntityHeadY(i);
                hp[i]      = shm.getEntityHp(i);
                hpMax[i]   = shm.getEntityHpMax(i);
                dist[i]    = shm.getEntityDistance(i);
                valid[i]   = shm.getEntityValid(i);
                names[i]   = shm.getEntityName(i);
            }
        }

        invalidate();
        handler.postDelayed(refresher, REFRESH_MS);
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        handler.removeCallbacks(refresher);
    }
}
