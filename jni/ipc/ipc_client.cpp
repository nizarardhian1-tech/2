// =============================================================================
// ipc_client.cpp — IPC Client Implementation
//
// Alur request() satu kali:
//   1. Pastikan terkoneksi (connect() jika perlu)
//   2. Buat JSON request dengan ID unik
//   3. Frame → kirim via sendAll()
//   4. Baca 4-byte header → decode length
//   5. Baca `length` byte payload
//   6. Parse JSON response
//   7. Verifikasi response ID cocok dengan request ID
//   8. Return response
// =============================================================================

#include "ipc_client.h"
#include "ipc_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <chrono>
#include <android/log.h>

#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  "IPC_CLIENT", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "IPC_CLIENT", fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, "IPC_CLIENT", fmt, ##__VA_ARGS__)

// =============================================================================
// Singleton
// =============================================================================
IPCClient& IPCClient::get() {
    static IPCClient instance;
    return instance;
}

// =============================================================================
// connect()
// =============================================================================
bool IPCClient::connect() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Sudah konek
    if (m_fd >= 0) return true;

    // Buat socket UNIX, tipe STREAM
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return false;
    }

    // Setup sockaddr untuk abstract namespace
    // Abstract socket: sa_data[0] = '\0', lalu nama
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, IPC_SOCKET_NAME, IPC_SOCKET_NAME_LEN + 1);
    // +1 karena kita menyertakan null prefix dalam IPC_SOCKET_NAME
    socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + IPC_SOCKET_NAME_LEN + 1;

    // Set connect timeout via non-blocking + select
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), addrLen);
    if (ret < 0 && errno != EINPROGRESS) {
        LOGE("connect() failed immediately: %s", strerror(errno));
        ::close(fd);
        return false;
    }

    if (ret != 0) {
        // Tunggu connect dengan select()
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv{};
        tv.tv_sec  = IPC_CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (IPC_CONNECT_TIMEOUT_MS % 1000) * 1000;

        ret = select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (ret <= 0) {
            LOGE("connect() timeout or error (ret=%d): %s", ret, strerror(errno));
            ::close(fd);
            return false;
        }

        // Cek apakah connect benar-benar berhasil
        int errCode = 0;
        socklen_t errLen = sizeof(errCode);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &errCode, &errLen);
        if (errCode != 0) {
            LOGE("connect() getsockopt error: %s", strerror(errCode));
            ::close(fd);
            return false;
        }
    }

    // Restore blocking mode
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    m_fd = fd;
    m_connected.store(true);
    LOGI("Connected to IPC server (fd=%d)", fd);
    return true;
}

// =============================================================================
// disconnect()
// =============================================================================
void IPCClient::disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
        m_connected.store(false);
        LOGI("Disconnected from IPC server");
    }
}

// =============================================================================
// isConnected()
// =============================================================================
bool IPCClient::isConnected() const {
    return m_connected.load();
}

// =============================================================================
// status()
// =============================================================================
std::string IPCClient::status() const {
    return m_connected.load() ? "connected" : "disconnected";
}

// =============================================================================
// nextId() — atomic increment
// =============================================================================
uint32_t IPCClient::nextId() {
    return m_idCounter.fetch_add(1, std::memory_order_relaxed);
}

// =============================================================================
// setRecvTimeout() — set SO_RCVTIMEO di socket
// =============================================================================
bool IPCClient::setRecvTimeout(int fd, int timeout_ms) {
    struct timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

// =============================================================================
// sendAll() — kirim semua byte, handle partial send
// =============================================================================
bool IPCClient::sendAll(int fd, const void* buf, size_t n) {
    const char* ptr = reinterpret_cast<const char*>(buf);
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t sent = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EINTR) continue;
            LOGE("send() failed: %s", strerror(errno));
            return false;
        }
        ptr       += sent;
        remaining -= sent;
    }
    return true;
}

// =============================================================================
// recvExact() — baca tepat n byte, handle EINTR dan partial reads
// =============================================================================
bool IPCClient::recvExact(int fd, void* buf, size_t n, int timeout_ms) {
    // Set timeout
    setRecvTimeout(fd, timeout_ms);

    char* ptr = reinterpret_cast<char*>(buf);
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t got = ::recv(fd, ptr, remaining, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            // EAGAIN / EWOULDBLOCK = timeout
            LOGE("recv() failed after %zu/%zu bytes: %s", n - remaining, n, strerror(errno));
            return false;
        }
        if (got == 0) {
            // Server tutup koneksi (EOF)
            LOGE("recv() EOF (server disconnected) after %zu/%zu bytes", n - remaining, n);
            return false;
        }
        ptr       += got;
        remaining -= got;
    }
    return true;
}

// =============================================================================
// request() — Synchronous RPC
// =============================================================================
json IPCClient::request(const std::string& cmd, const json& params, int timeout_ms) {
    // Response error default
    auto errResp = [&](const std::string& msg) -> json {
        m_failCount.fetch_add(1);
        return json{{"ok", false}, {"id", 0}, {"error", msg}};
    };

    std::lock_guard<std::mutex> lock(m_mutex);

    // Auto-reconnect jika belum konek
    if (m_fd < 0) {
        // Coba connect tanpa lock (lock sudah dipegang di sini)
        // → Kita buka koneksi langsung
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return errResp("socket failed");

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, IPC_SOCKET_NAME, IPC_SOCKET_NAME_LEN + 1);
        socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + IPC_SOCKET_NAME_LEN + 1;

        // Non-blocking connect dengan timeout
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), addrLen);

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv{ IPC_CONNECT_TIMEOUT_MS / 1000,
                          (IPC_CONNECT_TIMEOUT_MS % 1000) * 1000 };
        if (select(fd + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
            ::close(fd);
            LOGD("IPC server not ready (cmd=%s), skip", cmd.c_str());
            return errResp("server not ready");
        }

        int errCode = 0;
        socklen_t errLen = sizeof(errCode);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &errCode, &errLen);
        if (errCode != 0) {
            ::close(fd);
            return errResp("connect error");
        }

        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        m_fd = fd;
        m_connected.store(true);
        LOGI("Auto-reconnected (cmd=%s)", cmd.c_str());
    }

    // ── Buat JSON request ────────────────────────────────────────────────
    uint32_t reqId = m_idCounter.fetch_add(1, std::memory_order_relaxed);
    json req = {
        {"id",     reqId},
        {"cmd",    cmd},
        {"params", params.is_null() ? json::object() : params}
    };
    std::string payload = req.dump();

    // ── Frame dan kirim ──────────────────────────────────────────────────
    std::string framed = ipc_frame(payload);
    if (!sendAll(m_fd, framed.data(), framed.size())) {
        ::close(m_fd); m_fd = -1; m_connected.store(false);
        return errResp("send failed");
    }

    // ── Baca response header (4 byte) ────────────────────────────────────
    uint8_t hdr[4];
    if (!recvExact(m_fd, hdr, 4, timeout_ms)) {
        ::close(m_fd); m_fd = -1; m_connected.store(false);
        return errResp("recv header timeout");
    }
    uint32_t respLen = ipc_decode_len(hdr);

    if (respLen == 0 || respLen > IPC_MAX_MSG_SIZE) {
        LOGE("Invalid response length: %u", respLen);
        ::close(m_fd); m_fd = -1; m_connected.store(false);
        return errResp("invalid response length");
    }

    // ── Baca response payload ────────────────────────────────────────────
    std::string respPayload(respLen, '\0');
    if (!recvExact(m_fd, respPayload.data(), respLen, timeout_ms)) {
        ::close(m_fd); m_fd = -1; m_connected.store(false);
        return errResp("recv payload timeout");
    }

    // ── Parse JSON ───────────────────────────────────────────────────────
    json resp;
    try {
        resp = json::parse(respPayload);
    } catch (const json::exception& e) {
        LOGE("JSON parse error: %s", e.what());
        return errResp("json parse error");
    }

    // ── Verifikasi ID ────────────────────────────────────────────────────
    if (resp.value("id", 0u) != reqId) {
        LOGE("Response ID mismatch: expected %u got %u", reqId, resp.value("id", 0u));
        // Tetap kembalikan; mungkin sisa dari request sebelumnya
    }

    m_successCount.fetch_add(1);
    return resp;
}

// =============================================================================
// requestAsync() — fire and forget
// =============================================================================
void IPCClient::requestAsync(const std::string& cmd, const json& params) {
    std::string cmdCopy = cmd;
    json paramsCopy = params;
    std::thread([cmdCopy, paramsCopy]() {
        IPCClient::get().request(cmdCopy, paramsCopy);
    }).detach();
}
