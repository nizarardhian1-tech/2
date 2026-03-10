#include "trace/nullutils.h"

std::string NullUtils::removeNullChars(const std::string& str) {
    size_t nullPos = str.find('\0');
    if (nullPos != std::string::npos)
        return str.substr(0, nullPos);
    return str;
}

std::vector<uint8_t> NullUtils::interpretHex(std::string hex) {
    hex.erase(std::remove_if(hex.begin(), hex.end(), ::isspace), hex.end());
    if (hex.size() % 2 != 0) return {};
    for (char c : hex) {
        if (!std::isxdigit(c)) return {};
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

std::string NullUtils::bytesToHex(uint8_t* bytes, size_t len) {
    std::stringstream hexStream;
    for (size_t i = 0; i < len; i++)
        hexStream << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(bytes[i]) << " ";
    std::string outStr = hexStream.str();
    std::transform(outStr.begin(), outStr.end(), outStr.begin(), ::toupper);
    return outStr;
}

bool NullUtils::endsWith(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin());
}

int NullUtils::getApiLevel() {
    char sdk_version_str[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk", sdk_version_str);
    if (sdk_version_str[0] == '\0') return 0;
    return std::stoi(sdk_version_str);
}

// =============================================================================
// SELINUX helpers
//
// FIX: Binary injector sudah dijalankan sebagai root via "su -c ./injector".
// Jangan pakai "su -c setenforce" karena nested su tidak perlu dan bisa gagal
// di beberapa implementasi Magisk/KernelSU.
// Cukup panggil setenforce langsung — kita sudah punya UID 0.
// =============================================================================

void NullUtils::SELINUX_SetEnforce(int mode) {
    // Langsung — tanpa "su -c" karena binary sudah root
    if (mode == 0)      system("setenforce 0");
    else if (mode == 1) system("setenforce 1");
}

int NullUtils::SELINUX_GetEnforce() {
    std::ifstream file("/sys/fs/selinux/enforce");
    if (!file.is_open()) return -1;

    std::string line;
    std::getline(file, line);
    file.close();

    line = NullUtils::removeNullChars(line);
    if (line == "1") return 1;
    if (line == "0") return 0;
    return -1;
}
