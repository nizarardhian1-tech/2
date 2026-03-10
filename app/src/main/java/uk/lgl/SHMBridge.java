package uk.lgl;

import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;

/**
 * SHMBridge — Membaca data ESP dari shared memory file
 * yang ditulis oleh libinternal.so (C++ side).
 *
 * Format shared memory (harus sinkron dengan C++ SharedData struct):
 *   [0]     int32  magic    = 0xDEADBEEF
 *   [4]     int32  version  = 1
 *   [8]     int32  count    (jumlah entity)
 *   [12..N] EntityData[] array
 *
 * EntityData (per entity, 64 bytes):
 *   [0]  float x      screen X
 *   [4]  float y      screen Y
 *   [8]  float hp     current HP
 *   [12] float maxhp  max HP
 *   [16] int32 team   0=enemy 1=ally
 *   [20] int32 type   entity type
 *   [24] float dist   distance
 *   [28] int32 valid  1=valid 0=skip
 *   [32..63] reserved
 */
public class SHMBridge {

    public static final String SHM_PATH = "/data/data/com.mobiin.gp/files/mlbb_esp.shm";
    private static final int MAGIC      = 0xDEADBEEF;
    private static final int SHM_SIZE   = 4096; // 4KB, max ~60 entities

    // ── EntityData ─────────────────────────────────────────────────────────
    public static class EntityData {
        public float x, y;
        public float hp, maxHp;
        public int   team;   // 0=enemy, 1=ally
        public int   type;
        public float dist;
        public boolean valid;
    }

    // ── State ──────────────────────────────────────────────────────────────
    private MappedByteBuffer mMap;
    private boolean          mConnected = false;

    /**
     * Connect ke shared memory file.
     * @return true jika berhasil, false jika file belum ada
     */
    public boolean connect() {
        try {
            java.io.File f = new java.io.File(SHM_PATH);
            if (!f.exists()) return false;

            RandomAccessFile raf = new RandomAccessFile(f, "r");
            mMap = raf.getChannel().map(FileChannel.MapMode.READ_ONLY, 0, SHM_SIZE);
            mMap.order(ByteOrder.LITTLE_ENDIAN);

            // Verify magic
            int magic = mMap.getInt(0);
            if (magic != MAGIC) {
                raf.close();
                mMap = null;
                return false;
            }

            raf.close(); // Channel tetap valid via MappedByteBuffer
            mConnected = true;
            return true;
        } catch (Exception e) {
            mConnected = false;
            return false;
        }
    }

    public boolean isConnected() { return mConnected && mMap != null; }

    /**
     * Baca semua entity dari shared memory.
     * Thread-safe (read-only mmap).
     */
    public EntityData[] readEntities() {
        if (!isConnected()) return new EntityData[0];
        try {
            mMap.position(8);
            int count = mMap.getInt();
            if (count <= 0 || count > 64) return new EntityData[0];

            EntityData[] result = new EntityData[count];
            int base = 12;
            for (int i = 0; i < count; i++) {
                int off = base + i * 64;
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
        } catch (Exception e) {
            return new EntityData[0];
        }
    }
}
