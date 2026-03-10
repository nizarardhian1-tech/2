// =============================================================================
// ipc_server.cpp — IPC Server Implementation
//
// Flow per client connection:
//   acceptorLoop() → accept() → spawn clientHandler(fd) thread
//   clientHandler():
//     loop:
//       recvExact(4 bytes header) → decode length
//       recvExact(length bytes)   → parse JSON request
//       dispatchCommand(cmd, params) → handler fn
//       frame response JSON → sendAll()
//     until client disconnect or server stop
// =============================================================================

#include "ipc_server.h"
#include "ipc_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <android/log.h>

#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  "IPC_SERVER", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "IPC_SERVER", fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, "IPC_SERVER", fmt, ##__VA_ARGS__)

// =============================================================================
// Singleton
// =============================================================================
IPCServer& IPCServer::get() {
    static IPCServer instance;
    return instance;
}

// =============================================================================
// registerHandler()
// =============================================================================
void IPCServer::registerHandler(const std::string& cmd, HandlerFn handler) {
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    m_handlers[cmd] = std::move(handler);
    LOGD("Registered handler for cmd: %s", cmd.c_str());
}

// =============================================================================
// start()
// =============================================================================
bool IPCServer::start() {
    if (m_running.load()) {
        LOGI("Server already running");
        return true;
    }

    // Buat socket UNIX tipe STREAM
    m_serverFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_serverFd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return false;
    }

    // Reuse address
    int opt = 1;
    setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind ke abstract namespace
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    // IPC_SOCKET_NAME sudah mengandung '\0' prefix di awal
    memcpy(addr.sun_path, IPC_SOCKET_NAME, IPC_SOCKET_NAME_LEN + 1);
    socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + IPC_SOCKET_NAME_LEN + 1;

    if (::bind(m_serverFd, reinterpret_cast<struct sockaddr*>(&addr), addrLen) < 0) {
        LOGE("bind() failed: %s (mungkin sudah ada server lain?)", strerror(errno));
        ::close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    // Listen (max 5 koneksi antrian)
    if (::listen(m_serverFd, 5) < 0) {
        LOGE("listen() failed: %s", strerror(errno));
        ::close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    m_running.store(true);
    LOGI("IPC Server listening on abstract socket '@hybrid_imgui_ipc'");

    // Spawn acceptor thread
    m_acceptorThread = std::thread([this]() { acceptorLoop(); });
    m_acceptorThread.detach(); // Server hidup selama proses ada

    return true;
}

// =============================================================================
// stop()
// =============================================================================
void IPCServer::stop() {
    m_running.store(false);
    if (m_serverFd >= 0) {
        ::shutdown(m_serverFd, SHUT_RDWR);
        ::close(m_serverFd);
        m_serverFd = -1;
    }
    LOGI("IPC Server stopped");
}

// =============================================================================
// connectedClients()
// =============================================================================
int IPCServer::connectedClients() const {
    return m_connectedCount.load();
}

// =============================================================================
// acceptorLoop() — thread utama yang menerima koneksi baru
// =============================================================================
void IPCServer::acceptorLoop() {
    LOGI("Acceptor loop started (tid=%d)", gettid());

    while (m_running.load()) {
        // Gunakan select() dengan timeout agar bisa check m_running
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_serverFd, &rfds);
        struct timeval tv{ 1, 0 }; // 1 detik timeout

        int sel = select(m_serverFd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            LOGE("select() error: %s", strerror(errno));
            break;
        }
        if (sel == 0) continue; // timeout, cek m_running lagi

        // Ada koneksi masuk
        struct sockaddr_un clientAddr{};
        socklen_t clientAddrLen = sizeof(clientAddr);
        int clientFd = ::accept(m_serverFd,
                                reinterpret_cast<struct sockaddr*>(&clientAddr),
                                &clientAddrLen);
        if (clientFd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!m_running.load()) break;
            LOGE("accept() failed: %s", strerror(errno));
            continue;
        }

        LOGI("New client connected (fd=%d, total=%d)", clientFd,
             m_connectedCount.load() + 1);

        // Spawn handler thread per client
        // Thread di-detach agar tidak perlu di-join
        std::thread([this, clientFd]() {
            m_connectedCount.fetch_add(1);
            clientHandler(clientFd);
            m_connectedCount.fetch_sub(1);
            ::close(clientFd);
        }).detach();
    }

    LOGI("Acceptor loop ended");
}

// =============================================================================
// clientHandler() — menangani satu client dalam thread tersendiri
// =============================================================================
void IPCServer::clientHandler(int clientFd) {
    LOGD("clientHandler start (fd=%d)", clientFd);

    while (m_running.load()) {
        // ── Baca header (4 byte) ─────────────────────────────────────────
        uint8_t hdr[4];
        if (!recvExact(clientFd, hdr, 4, 60000)) {
            // Client disconnect atau timeout
            LOGI("Client fd=%d disconnected (recv header failed)", clientFd);
            break;
        }

        uint32_t msgLen = ipc_decode_len(hdr);
        if (msgLen == 0 || msgLen > IPC_MAX_MSG_SIZE) {
            LOGE("Invalid message length %u from fd=%d", msgLen, clientFd);
            break;
        }

        // ── Baca payload ─────────────────────────────────────────────────
        std::string payload(msgLen, '\0');
        if (!recvExact(clientFd, payload.data(), msgLen, 60000)) {
            LOGI("Client fd=%d disconnected (recv payload failed)", clientFd);
            break;
        }

        // ── Parse JSON request ────────────────────────────────────────────
        json req;
        try {
            req = json::parse(payload);
        } catch (const json::exception& e) {
            LOGE("JSON parse error from fd=%d: %s", clientFd, e.what());
            // Kirim error response dengan id=0
            json errResp = {{"id", 0}, {"ok", false}, {"error", "json parse error"}};
            std::string framed = ipc_frame(errResp.dump());
            sendAll(clientFd, framed.data(), framed.size());
            continue; // Tetap baca request berikutnya
        }

        uint32_t reqId   = req.value("id",  0u);
        std::string cmd  = req.value("cmd", std::string(""));
        json params      = req.value("params", json::object());

        LOGD("Request: id=%u cmd=%s", reqId, cmd.c_str());

        // ── Dispatch ──────────────────────────────────────────────────────
        json respData = dispatchCommand(cmd, params);

        // ── Bungkus dan kirim response ────────────────────────────────────
        json response = {
            {"id",  reqId},
            {"ok",  respData.value("ok", true)},
            {"data", respData.count("data")  ? respData["data"]  : json{}},
            {"error", respData.value("error", std::string(""))}
        };

        std::string respStr = ipc_frame(response.dump());
        if (!sendAll(clientFd, respStr.data(), respStr.size())) {
            LOGI("Client fd=%d disconnected (send response failed)", clientFd);
            break;
        }
    }

    LOGD("clientHandler end (fd=%d)", clientFd);
}

// =============================================================================
// dispatchCommand() — cari handler, panggil, tangkap exception
// =============================================================================
json IPCServer::dispatchCommand(const std::string& cmd, const json& params) {
    // Handler "ping" built-in
    if (cmd == "ping") {
        return {{"ok", true}, {"data", "pong"}};
    }

    HandlerFn handler;
    {
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        auto it = m_handlers.find(cmd);
        if (it == m_handlers.end()) {
            LOGE("Unknown command: %s", cmd.c_str());
            return {{"ok", false}, {"error", "unknown command: " + cmd}};
        }
        handler = it->second;
    }

    // Panggil handler, tangkap exception
    try {
        json result = handler(params);
        // Handler bisa return { "data": ... } atau { "ok": false, "error": ... }
        if (!result.contains("ok")) {
            result["ok"] = true; // Default ok=true jika tidak di-set
        }
        return result;
    } catch (const std::exception& e) {
        LOGE("Handler exception for cmd=%s: %s", cmd.c_str(), e.what());
        return {{"ok", false}, {"error", std::string(e.what())}};
    } catch (...) {
        LOGE("Handler unknown exception for cmd=%s", cmd.c_str());
        return {{"ok", false}, {"error", "unknown exception in handler"}};
    }
}

// =============================================================================
// sendAll() — kirim semua byte
// =============================================================================
bool IPCServer::sendAll(int fd, const void* buf, size_t n) {
    const char* ptr = reinterpret_cast<const char*>(buf);
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t sent = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        ptr       += sent;
        remaining -= sent;
    }
    return true;
}

// =============================================================================
// recvExact() — baca tepat n byte
// =============================================================================
bool IPCServer::recvExact(int fd, void* buf, size_t n, int timeout_ms) {
    // Set receive timeout
    struct timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char* ptr = reinterpret_cast<char*>(buf);
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t got = ::recv(fd, ptr, remaining, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false; // Timeout atau error
        }
        if (got == 0) return false; // EOF
        ptr       += got;
        remaining -= got;
    }
    return true;
}
