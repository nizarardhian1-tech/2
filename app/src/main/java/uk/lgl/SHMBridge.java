package uk.lgl;

import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;

/**
 * SHMBridge — Baca SharedData dari file SHM yang ditulis libinternal.so
 *
 * PATH DINAMIS: Tidak lagi hardcode ke satu game.
 *   Konstruksi path: /data/data/<gamePkg>/files/tool_esp.shm
 *   gamePkg dikirim dari OverlayMain.main(args[0]).
 *
 * Layout SharedData (harus sinkron dengan jni/shared_data.h):
 *   OFF  0: int32  version  (0x0004)
 *   OFF  4: bool   ready
 *   OFF  5: pad[3]
 *   OFF  8: int32  seq      (seqlock)
 *   OFF 12: int32  screenW
 *   OFF 16: int32  screenH
 *   OFF 20: float  fps
 *   OFF 24: bool   battleActive
 *   OFF 25: pad[3]
 *   OFF 28: ShmEntity[32]   (masing-masing 92 bytes)
 *   OFF 28 + 32*92 = 2972: int32 entityCount
 *   OFF 2976: ShmConfig
 *
 * ShmEntity layout (92 bytes):
 *   0:  float screenX, screenY    (8)
 *   8:  float headX, headY        (8)
 *   16: float worldX, worldY, worldZ (12)
 *   28: int   hp, hpMax           (8)
 *   36: char[32] name             (32)
 *   68: char[16] tag              (16)
 *   84: bool isValid              (1)
 *   85: bool canSight             (1)
 *   86: pad[2]                    (2)
 *   88: float distance            (4)
 *   total = 92
 *
 * ShmConfig (dari OFF_CONFIG):
 *   0: bool espLine
 *   1: bool espBox
 *   2: bool espHealth
 *   3: bool espName
 *   4: bool espDistance
 *   5: bool showFPS
 *   6: pad[2]
 *   8: float fov
 *   12: int colorIndex
 */
public class SHMBridge {

    public static final String SHM_FILENAME = "tool_esp.shm";
    public static final int    SHM_VERSION  = 0x0004;

    // SharedData offsets
    static final int OFF_VERSION    = 0;
    static final int OFF_READY      = 4;
    static final int OFF_SEQ        = 8;
    static final int OFF_SCREEN_W   = 12;
    static final int OFF_SCREEN_H   = 16;
    static final int OFF_FPS        = 20;
    static final int OFF_BATTLE     = 24;
    static final int OFF_ENTITIES   = 28;

    static final int ENTITY_SIZE    = 92;
    static final int MAX_ENTITIES   = 32;

    static final int OFF_ENTITY_COUNT = OFF_ENTITIES + ENTITY_SIZE * MAX_ENTITIES; // 2972
    static final int OFF_CONFIG       = OFF_ENTITY_COUNT + 4;                       // 2976

    // ShmEntity field offsets (relatif dari entity base)
    static final int ENT_SCREEN_X  = 0;
    static final int ENT_SCREEN_Y  = 4;
    static final int ENT_HEAD_X    = 8;
    static final int ENT_HEAD_Y    = 12;
    static final int ENT_WORLD_X   = 16;
    static final int ENT_WORLD_Y   = 20;
    static final int ENT_WORLD_Z   = 24;
    static final int ENT_HP        = 28;
    static final int ENT_HP_MAX    = 32;
    static final int ENT_NAME      = 36;  // char[32]
    static final int ENT_TAG       = 68;  // char[16]
    static final int ENT_IS_VALID  = 84;
    static final int ENT_CAN_SIGHT = 85;
    static final int ENT_DISTANCE  = 88;  // after 2 bytes pad

    // ShmConfig offsets (relatif dari OFF_CONFIG)
    static final int CFG_ESP_LINE    = 0;
    static final int CFG_ESP_BOX     = 1;
    static final int CFG_ESP_HEALTH  = 2;
    static final int CFG_ESP_NAME    = 3;
    static final int CFG_ESP_DIST    = 4;
    static final int CFG_SHOW_FPS    = 5;
    static final int CFG_FOV         = 8;   // float, after 2 bytes pad
    static final int CFG_COLOR_INDEX = 12;  // int

    // ── State ──────────────────────────────────────────────────────────────
    private final String mPath;
    private MappedByteBuffer mBuf;
    private boolean          mConnected = false;
    private int              mBufSize   = 0;
    private byte[]           mSnapshot;
    private ByteBuffer       mLocal;

    /**
     * @param gamePkg package name game target, misal "com.ohzegame.ramboshooter.brothersquad"
     *                Path SHM akan menjadi /data/data/<gamePkg>/files/tool_esp.shm
     */
    public SHMBridge(String gamePkg) {
        if (gamePkg != null && !gamePkg.isEmpty()) {
            // Strip process suffix jika ada (misal "com.pkg:UnityMain" → "com.pkg")
            int colon = gamePkg.indexOf(':');
            if (colon > 0) gamePkg = gamePkg.substring(0, colon);
            mPath = "/data/data/" + gamePkg + "/files/" + SHM_FILENAME;
        } else {
            mPath = "/data/local/tmp/" + SHM_FILENAME;
        }
    }

    /** Fallback constructor dengan path langsung */
    public SHMBridge() {
        mPath = "/data/local/tmp/" + SHM_FILENAME;
    }

    public String getPath() { return mPath; }

    // ── Connect ────────────────────────────────────────────────────────────
    public boolean connect() {
        RandomAccessFile f = null;
        try {
            f = new RandomAccessFile(mPath, "rw");
            long len = f.length();
            if (len < OFF_CONFIG + 16) { f.close(); return false; }
            mBuf  = f.getChannel().map(FileChannel.MapMode.READ_WRITE, 0, len);
            mBuf.order(ByteOrder.LITTLE_ENDIAN);
            mBufSize = (int) len;
            int ver = mBuf.getInt(OFF_VERSION);
            if (ver != SHM_VERSION) { f.close(); mBuf = null; return false; }
            mConnected = true;
            return true;
        } catch (Exception e) {
            return false;
        } finally {
            if (f != null) try { f.close(); } catch (Exception ignored) {}
        }
    }

    public boolean isConnected() { return mConnected && mBuf != null; }

    // ── Seqlock snapshot refresh ────────────────────────────────────────────
    public boolean refresh() {
        if (!isConnected()) return false;
        int cap = mBufSize;
        if (mSnapshot == null || mSnapshot.length < cap) {
            mSnapshot = new byte[cap];
            mLocal    = ByteBuffer.wrap(mSnapshot);
            mLocal.order(ByteOrder.LITTLE_ENDIAN);
        }
        for (int retry = 0; retry < 10; retry++) {
            int seq1 = mBuf.getInt(OFF_SEQ);
            if ((seq1 & 1) != 0) { try { Thread.sleep(0, 100000); } catch (Exception e) {} continue; }
            mBuf.position(0);
            mBuf.get(mSnapshot, 0, cap);
            int seq2 = mBuf.getInt(OFF_SEQ);
            if (seq1 == seq2) return true;
        }
        return false;
    }

    // ── Readers ────────────────────────────────────────────────────────────
    public boolean isReady()        { return mLocal != null && mLocal.get(OFF_READY) != 0; }
    public boolean isBattleActive() { return mLocal != null && mLocal.get(OFF_BATTLE) != 0; }
    public int     getScreenW()     { return mLocal != null ? mLocal.getInt(OFF_SCREEN_W) : 0; }
    public int     getScreenH()     { return mLocal != null ? mLocal.getInt(OFF_SCREEN_H) : 0; }
    public float   getFps()         { return mLocal != null ? mLocal.getFloat(OFF_FPS) : 0f; }
    public int     getEntityCount() { return mLocal != null ? mLocal.getInt(OFF_ENTITY_COUNT) : 0; }

    private int eOff(int i) { return OFF_ENTITIES + i * ENTITY_SIZE; }
    public float   getEntityScreenX(int i)  { return mLocal.getFloat(eOff(i) + ENT_SCREEN_X);  }
    public float   getEntityScreenY(int i)  { return mLocal.getFloat(eOff(i) + ENT_SCREEN_Y);  }
    public float   getEntityHeadX(int i)    { return mLocal.getFloat(eOff(i) + ENT_HEAD_X);    }
    public float   getEntityHeadY(int i)    { return mLocal.getFloat(eOff(i) + ENT_HEAD_Y);    }
    public float   getEntityWorldX(int i)   { return mLocal.getFloat(eOff(i) + ENT_WORLD_X);   }
    public float   getEntityWorldY(int i)   { return mLocal.getFloat(eOff(i) + ENT_WORLD_Y);   }
    public float   getEntityWorldZ(int i)   { return mLocal.getFloat(eOff(i) + ENT_WORLD_Z);   }
    public int     getEntityHp(int i)       { return mLocal.getInt  (eOff(i) + ENT_HP);        }
    public int     getEntityHpMax(int i)    { return mLocal.getInt  (eOff(i) + ENT_HP_MAX);    }
    public float   getEntityDistance(int i) { return mLocal.getFloat(eOff(i) + ENT_DISTANCE);  }
    public boolean getEntityValid(int i)    { return mLocal.get(eOff(i) + ENT_IS_VALID) != 0;  }
    public boolean getEntitySight(int i)    { return mLocal.get(eOff(i) + ENT_CAN_SIGHT) != 0; }
    public String  getEntityName(int i)     { return readCStr(eOff(i) + ENT_NAME, 32); }
    public String  getEntityTag(int i)      { return readCStr(eOff(i) + ENT_TAG, 16);  }

    // ── Config write (Java → SHM, dibaca libinternal) ──────────────────────
    public void setScreenSize(int w, int h) {
        if (isConnected()) { mBuf.putInt(OFF_SCREEN_W, w); mBuf.putInt(OFF_SCREEN_H, h); }
    }
    public void setEspLine(boolean v)    { if(isConnected()) mBuf.put(OFF_CONFIG+CFG_ESP_LINE,   (byte)(v?1:0)); }
    public void setEspBox(boolean v)     { if(isConnected()) mBuf.put(OFF_CONFIG+CFG_ESP_BOX,    (byte)(v?1:0)); }
    public void setEspHealth(boolean v)  { if(isConnected()) mBuf.put(OFF_CONFIG+CFG_ESP_HEALTH, (byte)(v?1:0)); }
    public void setEspName(boolean v)    { if(isConnected()) mBuf.put(OFF_CONFIG+CFG_ESP_NAME,   (byte)(v?1:0)); }
    public void setEspDistance(boolean v){ if(isConnected()) mBuf.put(OFF_CONFIG+CFG_ESP_DIST,   (byte)(v?1:0)); }
    public void setShowFPS(boolean v)    { if(isConnected()) mBuf.put(OFF_CONFIG+CFG_SHOW_FPS,   (byte)(v?1:0)); }
    public void setFov(float v)          { if(isConnected()) mBuf.putFloat(OFF_CONFIG+CFG_FOV, v); }
    public void setColorIndex(int v)     { if(isConnected()) mBuf.putInt(OFF_CONFIG+CFG_COLOR_INDEX, v); }

    private String readCStr(int off, int max) {
        if (mLocal == null) return "";
        StringBuilder sb = new StringBuilder();
        for (int j = 0; j < max; j++) {
            byte b = mLocal.get(off + j);
            if (b == 0) break;
            sb.append((char) b);
        }
        return sb.toString();
    }
}
