#pragma once
// =============================================================================
// ipc_server.h — IPC Server (dijalankan di sisi Internal: libinternal.so)
//
// IPCServer berjalan di dalam proses game (setelah di-inject via ptrace).
// Ia membuka abstract UNIX socket dan menunggu koneksi dari client (ImGui).
//
// DESAIN THREADING:
//   - 1 thread acceptor: menerima koneksi baru
//   - Per koneksi: 1 thread handler (atau bisa di-multiplex dengan poll)
//   - Thread handler: baca request → dispatch ke handler function → kirim response
//
// REGISTRASI HANDLER:
//   server.registerHandler("get_classes", [](const json& params) -> json {
//       return { {"ok", true}, {"data", Il2cpp::GetClassNames()} };
//   });
//   server.start();
//
// =============================================================================

#ifndef IPC_SERVER_H
#define IPC_SERVER_H

#include <string>
#include <functional>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

#include "json/single_include/nlohmann/json.hpp"
using json = nlohmann::json;

class IPCServer {
public:
    // Tipe handler: menerima params JSON, mengembalikan data JSON
    // Return value akan di-wrap otomatis menjadi { "ok": true, "data": ... }
    // Atau throw std::exception untuk error response
    using HandlerFn = std::function<json(const json& params)>;

    // ── Singleton ──────────────────────────────────────────────────────────
    static IPCServer& get();

    // ── Handler registration ───────────────────────────────────────────────
    /**
     * Daftarkan handler untuk command tertentu.
     * Panggil SEBELUM start().
     * Thread-safe: bisa dipanggil dari thread manapun.
     */
    void registerHandler(const std::string& cmd, HandlerFn handler);

    // ── Lifecycle ─────────────────────────────────────────────────────────
    /**
     * Mulai server: buat socket, bind, listen, spawn acceptor thread.
     * @return true jika berhasil bind
     */
    bool start();

    /**
     * Hentikan server dan bersihkan semua resource.
     */
    void stop();

    /**
     * @return true jika server sedang berjalan
     */
    bool isRunning() const { return m_running.load(); }

    /**
     * @return jumlah client yang sedang terkoneksi
     */
    int connectedClients() const;

private:
    IPCServer() = default;
    ~IPCServer() { stop(); }
    IPCServer(const IPCServer&) = delete;
    IPCServer& operator=(const IPCServer&) = delete;

    // ── Internal ──────────────────────────────────────────────────────────
    void acceptorLoop();
    void clientHandler(int clientFd);

    bool sendAll(int fd, const void* buf, size_t n);
    bool recvExact(int fd, void* buf, size_t n, int timeout_ms = 30000);

    json dispatchCommand(const std::string& cmd, const json& params);

    // ── State ─────────────────────────────────────────────────────────────
    int                 m_serverFd = -1;
    std::atomic<bool>   m_running{false};
    std::thread         m_acceptorThread;

    mutable std::mutex  m_handlerMutex;
    std::unordered_map<std::string, HandlerFn> m_handlers;

    mutable std::mutex  m_clientsMutex;
    std::vector<std::thread> m_clientThreads;
    std::atomic<int>    m_connectedCount{0};
};

#endif // IPC_SERVER_H
