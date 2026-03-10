#pragma once
// =============================================================================
// ipc_client.h — IPC Client (dijalankan di sisi External: libimgui_ext.so)
//
// IPCClient adalah thread-safe synchronous RPC client.
// Setiap request() membuka koneksi (jika belum) → kirim → tunggu response.
//
// DESAIN KONEKSI:
//   - Persistent connection: koneksi dipertahankan selama server ada
//   - Auto-reconnect: jika koneksi putus, otomatis reconnect pada request berikutnya
//   - Thread-safe: dilindungi mutex, aman dipanggil dari ImGui render thread
//   - Timeout: setiap request memiliki timeout agar tidak blok render loop
//
// PENGGUNAAN DARI ImGui:
//   IPCClient& ipc = IPCClient::get();
//   auto resp = ipc.request("get_classes", {{"image", "Assembly-CSharp"}});
//   if (resp["ok"]) { ... resp["data"] ... }
// =============================================================================

#ifndef IPC_CLIENT_H
#define IPC_CLIENT_H

#include <string>
#include <mutex>
#include <atomic>
#include <functional>

// nlohmann/json — include path disesuaikan dengan struktur project Proyek 2
// Ganti path ini jika lokasi json berbeda di project Anda
#include "json/single_include/nlohmann/json.hpp"
using json = nlohmann::json;

class IPCClient {
public:
    // ── Singleton ──────────────────────────────────────────────────────────
    static IPCClient& get();

    // ── Connection management ──────────────────────────────────────────────
    /**
     * Coba connect ke IPC Server.
     * @return true jika berhasil
     */
    bool connect();

    /**
     * Disconnect dan bersihkan socket.
     */
    void disconnect();

    /**
     * @return true jika socket sedang terhubung
     */
    bool isConnected() const;

    // ── RPC ───────────────────────────────────────────────────────────────
    /**
     * Kirim satu request dan tunggu response (synchronous, blocking).
     *
     * @param cmd     Nama command (lihat ipc_protocol.h)
     * @param params  Parameter JSON (opsional)
     * @param timeout_ms Timeout dalam milidetik (default: IPC_REQUEST_TIMEOUT_MS)
     * @return        Response JSON: { "ok": bool, "data": any, "error": string }
     *                Jika gagal total: { "ok": false, "error": "connection failed" }
     */
    json request(const std::string& cmd,
                 const json& params = nullptr,
                 int timeout_ms = 8000);

    // ── Async convenience (fire-and-forget untuk command tanpa return value) ─
    /**
     * Kirim command secara asinkron di thread terpisah.
     * Response diabaikan. Berguna untuk write_mem, hook_method, dll.
     */
    void requestAsync(const std::string& cmd, const json& params = nullptr);

    // ── Status ────────────────────────────────────────────────────────────
    /**
     * @return string status: "connected", "disconnected", "connecting"
     */
    std::string status() const;

    /**
     * @return jumlah request sukses sejak start
     */
    uint64_t successCount() const { return m_successCount; }

    /**
     * @return jumlah request yang gagal
     */
    uint64_t failCount() const { return m_failCount; }

private:
    IPCClient() = default;
    ~IPCClient() { disconnect(); }

    // Tidak boleh copy/move singleton
    IPCClient(const IPCClient&) = delete;
    IPCClient& operator=(const IPCClient&) = delete;

    // ── Internal helpers ──────────────────────────────────────────────────

    /**
     * Baca N byte dari socket, handle EINTR dan partial reads.
     * @return true jika berhasil baca tepat `n` byte
     */
    bool recvExact(int fd, void* buf, size_t n, int timeout_ms);

    /**
     * Kirim semua byte, handle partial sends.
     * @return true jika semua terkirim
     */
    bool sendAll(int fd, const void* buf, size_t n);

    /**
     * Set socket receive timeout.
     */
    bool setRecvTimeout(int fd, int timeout_ms);

    /**
     * Dapatkan ID unik untuk request berikutnya (atomic increment).
     */
    uint32_t nextId();

    // ── State ─────────────────────────────────────────────────────────────
    mutable std::mutex m_mutex;          // melindungi semua state di bawah
    int                m_fd      = -1;   // socket file descriptor (-1 = disconnected)
    std::atomic<bool>  m_connected{false};
    std::atomic<uint32_t> m_idCounter{1};
    std::atomic<uint64_t> m_successCount{0};
    std::atomic<uint64_t> m_failCount{0};
};

#endif // IPC_CLIENT_H
