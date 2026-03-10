#pragma once
// =============================================================================
// ipc_protocol.h — Hybrid ImGui IPC Protocol Definition
//
// ARSITEKTUR:
//   [External: libimgui_ext.so / app_process] <──UNIX Socket──> [Internal: libinternal.so / game]
//
// Framing:
//   Setiap message di-prefix dengan 4 byte (uint32_t LE) yang menyatakan
//   panjang payload JSON yang mengikutinya. Ini memastikan pembacaan atomik
//   bahkan jika kernel memecah send() menjadi beberapa read().
//
//   ┌─────────────────────────┬──────────────────────────────────────────────┐
//   │  4 bytes: uint32_t len  │  len bytes: UTF-8 JSON payload               │
//   └─────────────────────────┴──────────────────────────────────────────────┘
//
// Request JSON Schema:
//   { "id": <uint32>, "cmd": "<string>", "params": <object|null> }
//
// Response JSON Schema:
//   { "id": <uint32>, "ok": <bool>, "data": <any>, "error": "<string>" }
//
// COMMANDS YANG TERSEDIA:
// ─────────────────────────────────────────────────────────────────────────────
//  "ping"         → pong, cek koneksi
//  "get_images"   → list semua IL2CPP image name
//  "get_classes"  params: { "image": "Assembly-CSharp" }
//                 → list class { name, namespace, ptr }
//  "get_methods"  params: { "class_ptr": "0x..." }
//                 → list method { name, ptr, sig }
//  "get_fields"   params: { "class_ptr": "0x..." }
//                 → list field { name, type, offset }
//  "read_mem"     params: { "addr": "0x...", "size": 64 }
//                 → hex string dari bytes (misal: "AABBCC00...")
//  "write_mem"    params: { "addr": "0x...", "bytes": "AABBCC..." }
//                 → { "written": N }
//  "hook_method"  params: { "method_ptr": "0x...", "enable": true }
//                 → { "was_hooked": false }
//  "dump_classes" params: { "image": "Assembly-CSharp" }
//                 → full dump JSON untuk class browser
//  "get_lib_base" params: { "lib": "libil2cpp.so" }
//                 → { "base": "0x..." }
//  "set_value"    params: { "addr": "0x...", "type": "float|int|bool", "value": ... }
//                 → { "ok": true }
// =============================================================================

#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <cstdint>
#include <string>

// ── Socket path ──────────────────────────────────────────────────────────────
// Menggunakan Linux Abstract Namespace Socket (dimulai dengan '\0').
// Keuntungan:
//   1. Tidak perlu file di filesystem → tidak ada masalah permission lintas UID
//   2. Dibersihkan otomatis oleh kernel saat semua FD-nya ditutup
//   3. Tidak terpengaruh SELinux file context
//
// Sisi server (libinternal.so, berjalan di proses game) listen di sini.
// Sisi client (libimgui_ext.so, berjalan di app_process) connect ke sini.
// ─────────────────────────────────────────────────────────────────────────────
#define IPC_SOCKET_NAME  "\0hybrid_imgui_ipc"   // Abstract namespace (null prefix)
#define IPC_SOCKET_NAME_LEN 18                  // strlen (tidak termasuk '\0')
#define IPC_MAX_MSG_SIZE (4 * 1024 * 1024)      // 4 MB max per message (untuk dump besar)
#define IPC_CONNECT_TIMEOUT_MS 3000
#define IPC_REQUEST_TIMEOUT_MS 8000

// ── Framing helpers ───────────────────────────────────────────────────────────

/**
 * Encode message ke format framing: [4-byte length][payload]
 * @param payload  JSON string yang akan dikirim
 * @return         Wire bytes siap kirim
 */
static inline std::string ipc_frame(const std::string& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    // Little-endian
    char hdr[4] = {
        static_cast<char>( len        & 0xFF),
        static_cast<char>((len >>  8) & 0xFF),
        static_cast<char>((len >> 16) & 0xFF),
        static_cast<char>((len >> 24) & 0xFF),
    };
    return std::string(hdr, 4) + payload;
}

/**
 * Decode length header dari 4 byte pertama
 */
static inline uint32_t ipc_decode_len(const uint8_t* hdr) {
    return static_cast<uint32_t>(hdr[0])
         | (static_cast<uint32_t>(hdr[1]) <<  8)
         | (static_cast<uint32_t>(hdr[2]) << 16)
         | (static_cast<uint32_t>(hdr[3]) << 24);
}

// ── Command string constants ──────────────────────────────────────────────────
namespace IPC {
    static constexpr const char* CMD_PING         = "ping";
    static constexpr const char* CMD_GET_IMAGES   = "get_images";
    static constexpr const char* CMD_GET_CLASSES  = "get_classes";
    static constexpr const char* CMD_GET_METHODS  = "get_methods";
    static constexpr const char* CMD_GET_FIELDS   = "get_fields";
    static constexpr const char* CMD_READ_MEM     = "read_mem";
    static constexpr const char* CMD_WRITE_MEM    = "write_mem";
    static constexpr const char* CMD_HOOK_METHOD  = "hook_method";
    static constexpr const char* CMD_DUMP_CLASSES = "dump_classes";
    static constexpr const char* CMD_GET_LIB_BASE = "get_lib_base";
    static constexpr const char* CMD_SET_VALUE    = "set_value";
}

#endif // IPC_PROTOCOL_H
