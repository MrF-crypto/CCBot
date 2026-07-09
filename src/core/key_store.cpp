#include "core/key_store.h"
#include <windows.h>
#include <wincrypt.h>   // CryptProtectData / CryptUnprotectData (crypt32.lib)
#include <fstream>
#include <vector>

namespace ccbot {

// path 是 QString::toStdString() 产出的 UTF-8 字节。Windows 上 std::ofstream/ifstream
// 用 const char* 打开文件时是按当前 ANSI 代码页（而不是 UTF-8）解读的，这个 App 的数据目录
// 带中文（"CCG合约监控"），直接用 std::string 路径会把中文目录名解析成乱码路径、静默打开失败
// ——所以这里统一转成宽字符再走 MSVC 支持的 wchar_t* 重载。
static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

static std::vector<uint8_t> dpapi_enc(const std::string& data) {
    DATA_BLOB in  = { (DWORD)data.size(), (BYTE*)data.data() };
    DATA_BLOB out = {};
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE, &out))
        return {};
    std::vector<uint8_t> buf(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return buf;
}

static std::string dpapi_dec(const std::vector<uint8_t>& data) {
    DATA_BLOB in  = { (DWORD)data.size(), (BYTE*)const_cast<uint8_t*>(data.data()) };
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_LOCAL_MACHINE, &out))
        return {};
    std::string s((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

bool KeyStore::save(const Creds& c, const std::string& path) {
    std::string plain = c.api_key + "\n" + c.api_secret + "\n" +
                        (c.testnet ? "1" : "0");
    auto enc = dpapi_enc(plain);
    if (enc.empty()) return false;
    std::ofstream f(utf8_to_wide(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((char*)enc.data(), (std::streamsize)enc.size());
    return true;
}

bool KeyStore::load(Creds& c, const std::string& path) {
    std::ifstream f(utf8_to_wide(path), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> enc((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (enc.empty()) return false;
    std::string plain = dpapi_dec(enc);
    if (plain.empty()) return false;

    size_t p1 = plain.find('\n');
    if (p1 == std::string::npos) return false;
    size_t p2 = plain.find('\n', p1 + 1);
    if (p2 == std::string::npos) return false;

    c.api_key    = plain.substr(0, p1);
    c.api_secret = plain.substr(p1 + 1, p2 - p1 - 1);
    c.testnet    = (plain.substr(p2 + 1) == "1");
    return true;
}

} // namespace ccbot
