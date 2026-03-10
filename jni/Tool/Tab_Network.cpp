// ============================================================================
// Tool/Tab_Network.cpp
// Network Intercept: DNS · SSL · Socket
// Hook getaddrinfo, SSL_write, SSL_read, send, recv
// ============================================================================

#include "Tab_Network.h"
#include "../Includes/Logger.h"
#include "../Tool/Util.h"
#include "../Includes/obfuscate.h"
#include "../imgui/imgui.h"

#include <Dobby/include/dobby.h>
#include <dlfcn.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstring>
#include <ctime>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Tipe data satu log entry
// ─────────────────────────────────────────────────────────────────────────────
enum class NetType { DNS, SSL_SEND, SSL_RECV, SOCK_SEND, SOCK_RECV };

struct NetEntry {
    NetType     type;
    std::string label;   // DNS: domain | SSL/Socket: hex/text preview
    std::string detail;  // full hex dump
    int         size;    // bytes
    time_t      ts;
};

// ─────────────────────────────────────────────────────────────────────────────
// Storage
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<NetEntry> s_entries;
static std::mutex            s_mutex;
static bool                  s_hooksInstalled = false;
static bool                  s_capturing      = false;

// Filter flags
static bool s_showDNS      = true;
static bool s_showSSLSend  = true;
static bool s_showSSLRecv  = true;
static bool s_showSockSend = false;
static bool s_showSockRecv = false;
static int  s_maxEntries   = 200;

// Selected entry index untuk detail panel
static int  s_selected = -1;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: bytes → hex string (preview max 32 byte)
// ─────────────────────────────────────────────────────────────────────────────
static std::string ToHexPreview(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t show = std::min(len, (size_t)32);
    std::string out;
    out.reserve(show * 3);
    char buf[4];
    for (size_t i = 0; i < show; i++) {
        snprintf(buf, sizeof(buf), "%02X ", p[i]);
        out += buf;
    }
    if (len > 32) out += "...";
    return out;
}

static std::string ToHexFull(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    std::string out;
    char buf[8];
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) {
            snprintf(buf, sizeof(buf), "%04zX  ", i);
            out += buf;
        }
        snprintf(buf, sizeof(buf), "%02X ", p[i]);
        out += buf;
        if ((i + 1) % 16 == 0) out += "\n";
    }
    // ASCII sidebar sederhana
    out += "\n[ASCII] ";
    for (size_t i = 0; i < std::min(len, (size_t)256); i++) {
        char c = (char)p[i];
        out += (c >= 0x20 && c < 0x7f) ? c : '.';
    }
    if (len > 256) out += "...";
    return out;
}

static void AddEntry(NetType type, const char* label, const void* data, size_t len)
{
    if (!s_capturing) return;
    std::lock_guard<std::mutex> lk(s_mutex);
    if ((int)s_entries.size() >= s_maxEntries)
        s_entries.erase(s_entries.begin());

    NetEntry e;
    e.type   = type;
    e.label  = label ? label : "";
    e.detail = data ? ToHexFull(data, len) : "";
    e.size   = (int)len;
    e.ts     = time(nullptr);
    s_entries.push_back(std::move(e));
}

// ─────────────────────────────────────────────────────────────────────────────
// Hook: getaddrinfo — DNS queries
// ─────────────────────────────────────────────────────────────────────────────
using fn_getaddrinfo = int(*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
static fn_getaddrinfo o_getaddrinfo = nullptr;

static int hook_getaddrinfo(const char* node, const char* service,
    const struct addrinfo* hints, struct addrinfo** res)
{
    if (node && s_capturing) {
        LOGI("[NET/DNS] %s", node);
        AddEntry(NetType::DNS, node, nullptr, 0);
    }
    return o_getaddrinfo(node, service, hints, res);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hook: SSL_write — data dikirim game (plaintext sebelum enkripsi)
// ─────────────────────────────────────────────────────────────────────────────
using fn_ssl_write = int(*)(void* ssl, const void* buf, int num);
static fn_ssl_write o_ssl_write = nullptr;

static int hook_ssl_write(void* ssl, const void* buf, int num)
{
    if (buf && num > 0 && s_capturing) {
        std::string preview = ToHexPreview(buf, num);
        LOGI("[NET/SSL_SEND] %d bytes | %s", num, preview.c_str());
        AddEntry(NetType::SSL_SEND, ("SEND " + std::to_string(num) + " bytes").c_str(), buf, num);
    }
    return o_ssl_write(ssl, buf, num);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hook: SSL_read — data diterima game (plaintext setelah dekripsi)
// ─────────────────────────────────────────────────────────────────────────────
using fn_ssl_read = int(*)(void* ssl, void* buf, int num);
static fn_ssl_read o_ssl_read = nullptr;

static int hook_ssl_read(void* ssl, void* buf, int num)
{
    int result = o_ssl_read(ssl, buf, num);
    if (result > 0 && s_capturing) {
        std::string preview = ToHexPreview(buf, result);
        LOGI("[NET/SSL_RECV] %d bytes | %s", result, preview.c_str());
        AddEntry(NetType::SSL_RECV, ("RECV " + std::to_string(result) + " bytes").c_str(), buf, result);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hook: send — raw socket send
// ─────────────────────────────────────────────────────────────────────────────
using fn_send = ssize_t(*)(int, const void*, size_t, int);
static fn_send o_send = nullptr;

static ssize_t hook_send(int sockfd, const void* buf, size_t len, int flags)
{
    if (buf && len > 0 && s_capturing && s_showSockSend) {
        AddEntry(NetType::SOCK_SEND, ("SOCK_SEND " + std::to_string(len) + " bytes").c_str(), buf, len);
    }
    return o_send(sockfd, buf, len, flags);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hook: recv — raw socket recv
// ─────────────────────────────────────────────────────────────────────────────
using fn_recv = ssize_t(*)(int, void*, size_t, int);
static fn_recv o_recv = nullptr;

static ssize_t hook_recv(int sockfd, void* buf, size_t len, int flags)
{
    ssize_t result = o_recv(sockfd, buf, len, flags);
    if (result > 0 && s_capturing && s_showSockRecv) {
        AddEntry(NetType::SOCK_RECV, ("SOCK_RECV " + std::to_string(result) + " bytes").c_str(), buf, result);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cari SSL_write / SSL_read di semua library yang dimuat
// ─────────────────────────────────────────────────────────────────────────────
static void* FindSSLSymbol(const char* symbol)
{
    // Coba library umum dulu
    const char* sslLibs[] = {
        "libssl.so", "libssl.so.1.1", "libssl.so.3",
        "libboringssl.so", "libcrypto.so",
        nullptr
    };
    for (int i = 0; sslLibs[i]; i++) {
        void* h = dlopen(sslLibs[i], RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen(sslLibs[i], RTLD_NOW);
        if (h) {
            void* sym = dlsym(h, symbol);
            if (sym) {
                LOGI("[NET] Found %s in %s", symbol, sslLibs[i]);
                return sym;
            }
        }
    }

    // Scan semua .so di /proc/self/maps
    FILE* fp = fopen("/proc/self/maps", "r");
    if (fp) {
        char line[512];
        std::string prev;
        while (fgets(line, sizeof(line), fp)) {
            char* p = strchr(line, '/');
            if (!p || !strstr(line, ".so")) continue;
            p[strcspn(p, "\n")] = 0;
            if (prev == p) continue;
            prev = p;
            void* h = dlopen(p, RTLD_NOW | RTLD_NOLOAD);
            if (h) {
                void* sym = dlsym(h, symbol);
                if (sym) {
                    LOGI("[NET] Found %s in %s", symbol, p);
                    fclose(fp);
                    return sym;
                }
            }
        }
        fclose(fp);
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Init — pasang semua hooks
// ─────────────────────────────────────────────────────────────────────────────
void NetworkTab::Init()
{
    if (s_hooksInstalled) return;
    s_hooksInstalled = true;

    // DNS
    void* sym = dlsym(RTLD_DEFAULT, "getaddrinfo");
    if (sym) {
        int r = DobbyHook(sym, (void*)hook_getaddrinfo, (void**)&o_getaddrinfo);
        LOGI("[NET] Hook getaddrinfo → %s", r == 0 ? "OK" : "FAIL");
    }

    // SSL_write
    sym = FindSSLSymbol("SSL_write");
    if (sym) {
        int r = DobbyHook(sym, (void*)hook_ssl_write, (void**)&o_ssl_write);
        LOGI("[NET] Hook SSL_write → %s", r == 0 ? "OK" : "FAIL");
    } else {
        LOGW("[NET] SSL_write not found");
    }

    // SSL_read
    sym = FindSSLSymbol("SSL_read");
    if (sym) {
        int r = DobbyHook(sym, (void*)hook_ssl_read, (void**)&o_ssl_read);
        LOGI("[NET] Hook SSL_read → %s", r == 0 ? "OK" : "FAIL");
    } else {
        LOGW("[NET] SSL_read not found");
    }

    // send / recv (raw socket)
    sym = dlsym(RTLD_DEFAULT, "send");
    if (sym) DobbyHook(sym, (void*)hook_send, (void**)&o_send);

    sym = dlsym(RTLD_DEFAULT, "recv");
    if (sym) DobbyHook(sym, (void*)hook_recv, (void**)&o_recv);

    LOGI("[NET] NetworkTab::Init done");
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — UI Tab
// ─────────────────────────────────────────────────────────────────────────────
void NetworkTab::Draw()
{
    // ── Toolbar ──────────────────────────────────────────────
    if (s_capturing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 0.6f));
        if (ImGui::Button("  STOP  ")) s_capturing = false;
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.2f, 0.6f));
        if (ImGui::Button("  START ")) s_capturing = true;
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_entries.clear();
        s_selected = -1;
    }
    ImGui::SameLine();

    // Status indicator
    ImGui::PushStyleColor(ImGuiCol_Text,
        s_capturing ? ImVec4(0, 1, 0.5f, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1));
    ImGui::Text(s_capturing ? "● CAPTURING" : "○ IDLE");
    ImGui::PopStyleColor();

    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        ImGui::Text("| %zu entries", s_entries.size());
    }

    ImGui::Separator();

    // ── Filter checkboxes ─────────────────────────────────────
    ImGui::Text("Show:");
    ImGui::SameLine();
    ImGui::Checkbox("DNS",      &s_showDNS);      ImGui::SameLine();
    ImGui::Checkbox("SSL-OUT",     &s_showSSLSend);  ImGui::SameLine();
    ImGui::Checkbox("SSL-IN",     &s_showSSLRecv);  ImGui::SameLine();
    ImGui::Checkbox("Sock-OUT",    &s_showSockSend); ImGui::SameLine();
    ImGui::Checkbox("Sock-IN",    &s_showSockRecv);

    ImGui::Separator();

    // ── Split layout: list kiri | detail kanan ────────────────
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;
    float listW  = availW * 0.45f;
    float detailW = availW - listW - 8;

    // LIST
    ImGui::BeginChild("##netlist", ImVec2(listW, availH), true);
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        for (int i = (int)s_entries.size() - 1; i >= 0; i--) {
            auto& e = s_entries[i];

            // Filter
            if (e.type == NetType::DNS      && !s_showDNS)      continue;
            if (e.type == NetType::SSL_SEND  && !s_showSSLSend)  continue;
            if (e.type == NetType::SSL_RECV  && !s_showSSLRecv)  continue;
            if (e.type == NetType::SOCK_SEND && !s_showSockSend) continue;
            if (e.type == NetType::SOCK_RECV && !s_showSockRecv) continue;

            // Icon + warna per tipe
            ImVec4 col;
            const char* icon;
            switch (e.type) {
                case NetType::DNS:       col = {0.2f,0.8f,1.0f,1}; icon = "[DNS]"; break;
                case NetType::SSL_SEND:  col = {1.0f,0.5f,0.1f,1}; icon = "[SSL-OUT]"; break;
                case NetType::SSL_RECV:  col = {0.3f,1.0f,0.4f,1}; icon = "[SSL-IN]"; break;
                case NetType::SOCK_SEND: col = {1.0f,0.8f,0.2f,1}; icon = "[SKT_OUT]"; break;
                case NetType::SOCK_RECV: col = {0.7f,0.9f,0.3f,1}; icon = "[SKT_IN]"; break;
                default:                col = {1,1,1,1};            icon = "[???]"; break;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Text("%s", icon);
            ImGui::PopStyleColor();
            ImGui::SameLine();

            char id[64];
            snprintf(id, sizeof(id), "##item%d", i);
            bool selected = (s_selected == i);

            if (ImGui::Selectable(
                (e.label.size() > 28 ? e.label.substr(0,28) + "…" : e.label).c_str(),
                selected, 0, ImVec2(listW - 70, 0)))
            {
                s_selected = i;
            }

            if (e.type != NetType::DNS) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f,0.4f,0.4f,1));
                ImGui::Text("%dB", e.size);
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // DETAIL
    ImGui::BeginChild("##netdetail", ImVec2(detailW, availH), true);
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_selected >= 0 && s_selected < (int)s_entries.size()) {
            auto& e = s_entries[s_selected];

            // Header
            ImVec4 col;
            switch (e.type) {
                case NetType::DNS:       col = {0.2f,0.8f,1.0f,1}; break;
                case NetType::SSL_SEND:  col = {1.0f,0.5f,0.1f,1}; break;
                case NetType::SSL_RECV:  col = {0.3f,1.0f,0.4f,1}; break;
                default:                col = {1,0.8f,0.2f,1};     break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextWrapped("%s", e.label.c_str());
            ImGui::PopStyleColor();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f,0.5f,0.6f,1));
            ImGui::Text("Size: %d bytes  |  Time: %lld", e.size, (long long)e.ts);
            ImGui::PopStyleColor();

            // ── Tombol Copy & Export ─────────────────────────
            if (ImGui::Button("Copy Label")) {
                ImGui::SetClipboardText(e.label.c_str());
            }
            if (e.type != NetType::DNS && !e.detail.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Copy Hex")) {
                    ImGui::SetClipboardText(e.detail.c_str());
                }
                ImGui::SameLine();
                if (ImGui::Button("Export Entry")) {
                    // Simpan entry ke file di cache game
                    // Pakai path yang sama dengan fitur lain: IL2CPPTOOLS/network/
                    static std::string netDir = Util::RootDir() + "/network";
                    Util::EnsureDir(netDir);
                    char path[256];
                    snprintf(path, sizeof(path),
                        "%s/net_%lld.txt", netDir.c_str(), (long long)e.ts);
                    FILE* f = fopen(path, "w");
                    if (f) {
                        fprintf(f, "Label: %s\n", e.label.c_str());
                        fprintf(f, "Size: %d bytes\n", e.size);
                        fprintf(f, "Time: %lld\n\n", (long long)e.ts);
                        fprintf(f, "%s\n", e.detail.c_str());
                        fclose(f);
                        // Toast via ImGui tooltip sementara
                        ImGui::SetTooltip("Saved: %s", path);
                    }
                }
            }

            // Export ALL entries ke satu file
            ImGui::SameLine();
            if (ImGui::Button("Export ALL")) {
                char path[256];
                static std::string netDir2 = Util::RootDir() + "/network";
                Util::EnsureDir(netDir2);
                snprintf(path, sizeof(path), "%s/net_dump_all.txt", netDir2.c_str());
                FILE* f = fopen(path, "w");
                if (f) {
                    // Tidak bisa lock di sini (sudah di-lock), tulis langsung
                    for (auto& en : s_entries) {
                        const char* t = "?";
                        switch(en.type) {
                            case NetType::DNS:       t = "DNS";     break;
                            case NetType::SSL_SEND:  t = "SSL_OUT"; break;
                            case NetType::SSL_RECV:  t = "SSL_IN";  break;
                            case NetType::SOCK_SEND: t = "SKT_OUT"; break;
                            case NetType::SOCK_RECV: t = "SKT_IN";  break;
                        }
                        fprintf(f, "[%s] %s (%d bytes)\n", t, en.label.c_str(), en.size);
                        if (!en.detail.empty())
                            fprintf(f, "%s\n\n", en.detail.c_str());
                    }
                    fclose(f);
                    ImGui::SetTooltip("Saved: %s", path);
                }
            }

            ImGui::Separator();

            if (e.type == NetType::DNS) {
                ImGui::TextWrapped("Domain: %s", e.label.c_str());
            } else if (!e.detail.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f,0.9f,0.75f,1));
                ImGui::InputTextMultiline("##hexdump",
                    (char*)e.detail.c_str(), e.detail.size() + 1,
                    ImVec2(-1, -1),
                    ImGuiInputTextFlags_ReadOnly);
                ImGui::PopStyleColor();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f,0.4f,0.5f,1));
            ImGui::TextUnformatted("← Select an entry to see details");
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}
