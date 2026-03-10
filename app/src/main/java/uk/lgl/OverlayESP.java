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
 * OverlayESP — View fullscreen transparan yang menggambar ESP
 * menggunakan Android Canvas API.
 *
 * Data dibaca dari SHMBridge (shared memory dari libinternal.so).
 * Refresh setiap 50ms (20 FPS) — cukup untuk ESP tanpa boros baterai.
 *
 * Window stack:
 *   [Game Surface]
 *   [OverlayESP]     ← ini (FLAG_NOT_TOUCHABLE, di bawah ImGui)
 *   [ImGui Surface]  ← dihandle di OverlayMain
 */
public class OverlayESP extends View {

    private static final long REFRESH_MS = 50; // 20 FPS

    // ── Paint objects ──────────────────────────────────────────────────────
    private final Paint paintBox;
    private final Paint paintBoxAlly;
    private final Paint paintText;
    private final Paint paintHP;
    private final Paint paintHPBg;
    private final Paint paintLine;

    // ── State ──────────────────────────────────────────────────────────────
    private final SHMBridge shm;
    private SHMBridge.EntityData[] entities = new SHMBridge.EntityData[0];

    private final Handler  handler   = new Handler(Looper.getMainLooper());
    private final Runnable refresher = this::refreshAndDraw;

    // =========================================================================
    public OverlayESP(Context ctx, SHMBridge shm) {
        super(ctx);
        this.shm = shm;

        // Enemy box — merah
        paintBox = new Paint(Paint.ANTI_ALIAS_FLAG);
        paintBox.setColor(Color.rgb(255, 50, 50));
        paintBox.setStyle(Paint.Style.STROKE);
        paintBox.setStrokeWidth(2f);

        // Ally box — hijau
        paintBoxAlly = new Paint(Paint.ANTI_ALIAS_FLAG);
        paintBoxAlly.setColor(Color.rgb(50, 255, 100));
        paintBoxAlly.setStyle(Paint.Style.STROKE);
        paintBoxAlly.setStrokeWidth(2f);

        // Text info
        paintText = new Paint(Paint.ANTI_ALIAS_FLAG);
        paintText.setColor(Color.WHITE);
        paintText.setTextSize(28f);
        paintText.setTypeface(Typeface.MONOSPACE);
        paintText.setShadowLayer(3f, 1f, 1f, Color.BLACK);

        // HP bar background
        paintHPBg = new Paint();
        paintHPBg.setColor(Color.argb(180, 0, 0, 0));
        paintHPBg.setStyle(Paint.Style.FILL);

        // HP bar fill
        paintHP = new Paint();
        paintHP.setColor(Color.rgb(50, 220, 50));
        paintHP.setStyle(Paint.Style.FILL);

        // Line to enemy
        paintLine = new Paint(Paint.ANTI_ALIAS_FLAG);
        paintLine.setColor(Color.argb(120, 255, 50, 50));
        paintLine.setStrokeWidth(1f);

        // Start refresh loop
        setWillNotDraw(false);
        handler.postDelayed(refresher, REFRESH_MS);
    }

    // =========================================================================
    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (entities == null || entities.length == 0) return;

        float screenCX = getWidth()  / 2f;
        float screenCY = getHeight() / 2f;

        for (SHMBridge.EntityData e : entities) {
            if (e == null || !e.valid) continue;

            float sx = e.x;
            float sy = e.y;

            // Skip jika di luar layar
            if (sx < 0 || sx > getWidth() || sy < 0 || sy > getHeight()) {
                // Gambar arrow di tepi jika enemy
                if (e.team == 0) drawEdgeArrow(canvas, sx, sy, screenCX, screenCY);
                continue;
            }

            Paint boxPaint = (e.team == 0) ? paintBox : paintBoxAlly;

            // Estimasi ukuran box berdasarkan jarak
            float boxH = Math.max(20f, 200f - e.dist * 0.5f);
            float boxW = boxH * 0.5f;

            float left  = sx - boxW / 2f;
            float top   = sy - boxH;
            float right = sx + boxW / 2f;
            float bot   = sy;

            // 1. Bounding box
            canvas.drawRect(left, top, right, bot, boxPaint);

            // 2. HP bar di bawah box
            float hpRatio = (e.maxHp > 0) ? Math.min(1f, e.hp / e.maxHp) : 0f;
            canvas.drawRect(left, bot + 4, right, bot + 10, paintHPBg);
            paintHP.setColor(hpColor(hpRatio));
            canvas.drawRect(left, bot + 4, left + boxW * hpRatio, bot + 10, paintHP);

            // 3. Text: jarak
            String info = String.format("%.0fm", e.dist);
            canvas.drawText(info, left, top - 6, paintText);

            // 4. Line dari tengah bawah layar ke enemy
            if (e.team == 0) {
                canvas.drawLine(screenCX, screenCY, sx, sy, paintLine);
            }
        }
    }

    private void drawEdgeArrow(Canvas canvas, float tx, float ty, float cx, float cy) {
        // Gambar titik di tepi layar sebagai indikator arah
        float angle = (float) Math.atan2(ty - cy, tx - cx);
        float ex = cx + (float) Math.cos(angle) * (getWidth() / 2f - 20);
        float ey = cy + (float) Math.sin(angle) * (getHeight() / 2f - 20);
        ex = Math.max(10, Math.min(getWidth() - 10, ex));
        ey = Math.max(10, Math.min(getHeight() - 10, ey));
        canvas.drawCircle(ex, ey, 8f, paintBox);
    }

    private int hpColor(float ratio) {
        if (ratio > 0.6f) return Color.rgb(50, 220, 50);   // hijau
        if (ratio > 0.3f) return Color.rgb(255, 200, 0);   // kuning
        return Color.rgb(255, 60, 60);                      // merah
    }

    // =========================================================================
    private void refreshAndDraw() {
        if (!isAttachedToWindow()) return;
        // Baca data baru dari SHM
        if (shm != null && shm.isConnected()) {
            entities = shm.readEntities();
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
