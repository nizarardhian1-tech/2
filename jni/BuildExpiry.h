#pragma once
// =============================================================================
// BuildExpiry.h — External build expiry
// Anti-rollback: pakai CLOCK_BOOTTIME (tidak bisa dimanipulasi tanpa root)
// cross-check dengan wall clock untuk deteksi jam dimundurkan
//
// Cara pakai:
//   1. Set EXPIRE_DAYS sebelum build
//   2. Panggil CheckExpiry() di awal main()
//   3. Jika return false → exit
// =============================================================================
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <time.h>
#include <unistd.h>

// ── Konfigurasi ──────────────────────────────────────────────────────────────
#ifndef EXPIRE_DAYS
#define EXPIRE_DAYS 1
#endif

// Toleransi perbedaan boottime vs wallclock (detik)
// Kalau selisih > ini → dianggap jam dimundurkan
#define ROLLBACK_TOLERANCE_SEC 120

// XOR key obfuscate file
#define EXT_XOR_KEY 0xD4E8F2A6B3C7E1D5ULL

// =============================================================================
// PARSE BUILD DATE dari __DATE__ macro
// =============================================================================
static time_t _EXT_ParseBuildDate() {
    static const char* kMon[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    char mon[4] = {}; int day = 0, year = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return 0;
    int m = 0;
    for (int i = 0; i < 12; i++)
        if (strncmp(mon, kMon[i], 3) == 0) { m = i; break; }
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = m;
    t.tm_mday = day;
    return mktime(&t);
}

// =============================================================================
// BOOTTIME — waktu sejak boot, TIDAK terpengaruh perubahan jam sistem
// Ini kunci utama anti-rollback tanpa root
// =============================================================================
static int64_t _GetBoottime() {
    struct timespec ts = {};
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec;
}

// =============================================================================
// PATH & STORAGE
// File disimpan di lokasi yang tidak obvious, nama file seperti cache biasa
// =============================================================================
static std::string _EXT_GetStoragePath(int slot) {
    // Simpan di /data/local/tmp dengan nama tidak mencurigakan
    // Dua slot berbeda — keduanya harus dihapus untuk akali
    if (slot == 0) return "/storage/emulated/0/Android/data/com.mobiin.gp/files/dragon2017/assets/UnityData_NEW/Managed/etc/mono/.sys_cache_v2";
    return         "/data/user/0/com.mobiin.gp/files/.drv_state_v1";
}

// Format record: [wall_time: 8 byte][boot_offset: 8 byte] → XOR obfuscated
struct _EXT_Record {
    int64_t wallTime;    // wall clock saat terakhir jalan
    int64_t bootOffset;  // CLOCK_BOOTTIME saat terakhir jalan
};

static bool _EXT_ReadRecord(const std::string& path, _EXT_Record& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint64_t raw[2] = {};
    size_t n = fread(raw, sizeof(uint64_t), 2, f);
    fclose(f);
    if (n != 2) return false;
    out.wallTime   = (int64_t)(raw[0] ^ EXT_XOR_KEY);
    out.bootOffset = (int64_t)(raw[1] ^ (EXT_XOR_KEY ^ 0xFFFFFFFFFFFFFFFFULL));
    // Sanity check — nilai tidak masuk akal → anggap corrupt
    if (out.wallTime < 1700000000LL || out.wallTime > 9999999999LL) return false;
    if (out.bootOffset < 0 || out.bootOffset > 86400LL * 365 * 10)   return false;
    return true;
}

static void _EXT_WriteRecord(const std::string& path, int64_t wall, int64_t boot) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint64_t raw[2] = {
        (uint64_t)wall ^ EXT_XOR_KEY,
        (uint64_t)boot ^ (EXT_XOR_KEY ^ 0xFFFFFFFFFFFFFFFFULL)
    };
    fwrite(raw, sizeof(uint64_t), 2, f);
    fflush(f);
    fclose(f);
}

// =============================================================================
// LOGIKA UTAMA
// =============================================================================

// Ambil record terbaik dari 2 slot (ambil wallTime terbesar = yang paling terkini)
static bool _EXT_ReadBestRecord(_EXT_Record& out) {
    _EXT_Record r0 = {}, r1 = {};
    bool ok0 = _EXT_ReadRecord(_EXT_GetStoragePath(0), r0);
    bool ok1 = _EXT_ReadRecord(_EXT_GetStoragePath(1), r1);
    if (!ok0 && !ok1) return false;
    if (!ok0) { out = r1; return true; }
    if (!ok1) { out = r0; return true; }
    // Ambil yang wallTime lebih besar
    out = (r0.wallTime > r1.wallTime) ? r0 : r1;
    return true;
}

static void _EXT_WriteAllSlots(int64_t wall, int64_t boot) {
    _EXT_WriteRecord(_EXT_GetStoragePath(0), wall, boot);
    _EXT_WriteRecord(_EXT_GetStoragePath(1), wall, boot);
}

// =============================================================================
// CheckExpiry()
// Return true  = masih valid, boleh lanjut
// Return false = expired atau terdeteksi rollback → caller harus exit
// =============================================================================
static bool CheckExpiry() {
    time_t buildDate = _EXT_ParseBuildDate();
    if (buildDate == 0) return false; // gagal parse → safe default: expired

    time_t expiry = buildDate + (time_t)EXPIRE_DAYS * 86400LL;
    int64_t nowWall = (int64_t)time(nullptr);
    int64_t nowBoot = _GetBoottime();

    // ── 1. Cek expired dari wall clock ───────────────────────────────────────
    if (nowWall > expiry) return false;

    // ── 2. Baca record tersimpan ──────────────────────────────────────────────
    _EXT_Record rec = {};
    bool hasRecord  = _EXT_ReadBestRecord(rec);

    if (hasRecord) {
        // ── 3. Anti-rollback wall clock ──────────────────────────────────────
        // Jika wall clock sekarang lebih kecil dari yang pernah tersimpan → rollback
        if (nowWall < rec.wallTime) return false;

        // ── 4. Anti-rollback via CLOCK_BOOTTIME ──────────────────────────────
        // Logika:
        //   elapsed_boot = nowBoot - rec.bootOffset  → waktu nyata yang berlalu sejak sesi terakhir
        //   elapsed_wall = nowWall - rec.wallTime     → waktu wall clock yang berlalu
        //
        // Kalau user mundurkan jam HP:
        //   elapsed_wall akan jauh lebih kecil dari elapsed_boot
        //   (CLOCK_BOOTTIME jujur, wall clock dibohongi)
        //
        // Reboot → boottime reset ke 0, elapsed_boot bisa negatif → skip cek ini
        if (nowBoot >= 0 && rec.bootOffset >= 0 && nowBoot >= rec.bootOffset) {
            int64_t elapsedBoot = nowBoot - rec.bootOffset;
            int64_t elapsedWall = nowWall - rec.wallTime;

            // Jika wall clock tertinggal lebih dari toleransi dari boottime → rollback
            if (elapsedBoot - elapsedWall > ROLLBACK_TOLERANCE_SEC) return false;
        }
    }

    // ── 5. Update record dengan timestamp saat ini ────────────────────────────
    _EXT_WriteAllSlots(nowWall, nowBoot >= 0 ? nowBoot : 0);

    return true;
}

// =============================================================================
// DaysLeft() — untuk tampil di menu kalau mau
// =============================================================================
static int ExpiryDaysLeft() {
    time_t buildDate = _EXT_ParseBuildDate();
    if (buildDate == 0) return 0;
    time_t expiry  = buildDate + (time_t)EXPIRE_DAYS * 86400LL;
    time_t now     = time(nullptr);
    long   diff    = (long)(expiry - now);
    return diff > 0 ? (int)(diff / 86400) : 0;
}
