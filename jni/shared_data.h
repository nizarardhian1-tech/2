#pragma once
// =============================================================================
// shared_data.h — Shared Memory bridge: libinternal.so <-> OverlayMain
//
// File ini ada di DUA tempat:
//   jni/shared_data.h          (dibaca internal_main.cpp)
//   jni/shared_data.h          (dibaca SHMBridge.java via layout offset manual)
//
// PATH: /data/data/<game_pkg>/files/tool_esp.shm
//   Ditulis libinternal.so saat berjalan di dalam proses game.
//   Dibaca OverlayMain (app_process) yang tahu path karena menerima pkg via argv.
//
// PENTING: Tidak ada offset hardcoded game. Ini generic IL2Cpp tool.
//   - ESP data: KOSONG sampai user implement collector per-game di GameClass.h
//   - ready + version digunakan overlay untuk validasi koneksi
// =============================================================================

#include <cstdint>
#include <cstring>

#define SHM_VERSION  0x0004
#define SHM_FILENAME "tool_esp.shm"

#define MAX_PLAYERS   16
#define MAX_ENTITIES  32

// ---------------------------------------------------------------------------
// Generic entity (player / enemy)
// Ditulis libinternal.so, dibaca OverlayMain
// ---------------------------------------------------------------------------
struct ShmEntity {
    float screenX, screenY;     // root position (sudah W2S)
    float headX,   headY;       // head position (sudah W2S)
    float worldX,  worldY, worldZ;
    int   hp, hpMax;
    char  name[32];
    char  tag[16];              // "player" / "enemy" / "boss" / dll
    bool  isValid;
    bool  canSight;
    float distance;
    uint8_t _pad[2];
};

// ---------------------------------------------------------------------------
// Config — ditulis OverlayMain (Java), dibaca libinternal.so
// ---------------------------------------------------------------------------
struct ShmConfig {
    bool espLine;
    bool espBox;
    bool espHealth;
    bool espName;
    bool espDistance;
    bool showFPS;
    uint8_t _pad[2];
    float fov;
    int   colorIndex;  // 0=White,1=Red,2=Blue,3=Yellow,4=Cyan,5=Green
};

// ---------------------------------------------------------------------------
// Main shared block
// ---------------------------------------------------------------------------
struct SharedData {
    uint32_t version;          // SHM_VERSION — validasi pertama
    bool     ready;            // libinternal sudah init dan running
    uint8_t  _pad1[3];

    // Seqlock — internal increment sebelum+sesudah nulis data
    // Overlay: tunggu genap, baca, cek sama → data valid
    volatile uint32_t seq;

    // Screen info — ditulis libinternal (dari Screen.width/height),
    // bisa juga ditulis overlay saat pertama connect
    int   screenW, screenH;
    float fps;
    bool  battleActive;
    uint8_t _pad2[3];

    // Entity data
    ShmEntity entities[MAX_ENTITIES];
    int       entityCount;

    // Config (ditulis overlay, dibaca internal)
    ShmConfig config;
};
