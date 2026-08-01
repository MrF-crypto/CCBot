#include "core/key_store.h"

// 平台加密后端：
//   Windows —— DPAPI（CryptProtectData），密文绑定当前 Windows 用户
//   macOS   —— 系统钥匙串（Keychain，Security.framework），条目归属当前 macOS 用户
//   其他    —— 明文文件兜底（GUI 目前只发 Windows/macOS，这个分支只为编译完整性）
// 三个后端对外接口一致：save/load 打包格式都是 "key\nsecret\ntestnet"，互不兼容属预期
// （换电脑/换系统本来就要求重新输入密钥，见 README 迁移说明）。

#if defined(_WIN32)
// ─── Windows: DPAPI ───────────────────────────────────────────────────────────
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
    // flags=0：密文绑定当前 Windows 用户（同机器其他账户解不开）。
    // 旧版本用过 CRYPTPROTECT_LOCAL_MACHINE（本机任何账户可解），解密时 DPAPI 按
    // 密文自带的元数据处理，所以旧密文照常能读，只是新保存的从此收紧为用户级
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return {};
    std::vector<uint8_t> buf(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return buf;
}

static std::string dpapi_dec(const std::vector<uint8_t>& data) {
    DATA_BLOB in  = { (DWORD)data.size(), (BYTE*)const_cast<uint8_t*>(data.data()) };
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return {};
    std::string s((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

static bool write_blob(const std::string& path, const std::vector<uint8_t>& enc) {
    std::ofstream f(utf8_to_wide(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((char*)enc.data(), (std::streamsize)enc.size());
    return true;
}

static bool read_blob(const std::string& path, std::vector<uint8_t>& enc) {
    std::ifstream f(utf8_to_wide(path), std::ios::binary);
    if (!f) return false;
    enc.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return !enc.empty();
}

static bool backend_save(const std::string& plain, const std::string& path) {
    auto enc = dpapi_enc(plain);
    if (enc.empty()) return false;
    return write_blob(path, enc);
}

static bool backend_load(std::string& plain, const std::string& path) {
    std::vector<uint8_t> enc;
    if (!read_blob(path, enc)) return false;
    plain = dpapi_dec(enc);
    return !plain.empty();
}

} // namespace ccbot

#elif defined(__APPLE__)
// ─── macOS: 系统钥匙串（Keychain）──────────────────────────────────────────────
// 密文不落自定义文件，直接存进当前用户的钥匙串（kSecClassGenericPassword 条目，
// service=com.ccbot.CCGMonitor）。path 参数在本后端里不使用。
// 首次访问时 macOS 可能弹出钥匙串授权框，选"始终允许"即可。
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>

namespace ccbot {

static const CFStringRef kService = CFSTR("com.ccbot.CCGMonitor");
static const CFStringRef kAccount = CFSTR("binance-api");

static CFMutableDictionaryRef base_query() {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass,       kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, kService);
    CFDictionarySetValue(q, kSecAttrAccount, kAccount);
    return q;
}

static bool backend_save(const std::string& plain, const std::string& /*path*/) {
    // 先删旧条目再新增（SecItemUpdate 的语义更繁琐，删+增等效且简单）
    CFMutableDictionaryRef del = base_query();
    SecItemDelete(del);
    CFRelease(del);

    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                  (const UInt8*)plain.data(), (CFIndex)plain.size());
    if (!data) return false;

    CFMutableDictionaryRef add = base_query();
    CFDictionarySetValue(add, kSecValueData, data);
    OSStatus st = SecItemAdd(add, nullptr);
    CFRelease(add);
    CFRelease(data);
    return st == errSecSuccess;
}

static bool backend_load(std::string& plain, const std::string& /*path*/) {
    CFMutableDictionaryRef q = base_query();
    CFDictionarySetValue(q, kSecReturnData,  kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit,  kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    OSStatus st = SecItemCopyMatching(q, &result);
    CFRelease(q);
    if (st != errSecSuccess || !result) return false;

    CFDataRef data = (CFDataRef)result;
    plain.assign((const char*)CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));
    CFRelease(result);
    return !plain.empty();
}

} // namespace ccbot

#else
// ─── 其他平台：明文文件兜底（仅为编译完整性，GUI 不在这些平台发布）────────────
#include <fstream>

namespace ccbot {

static bool backend_save(const std::string& plain, const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(plain.data(), (std::streamsize)plain.size());
    return true;
}

static bool backend_load(std::string& plain, const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    plain.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return !plain.empty();
}

} // namespace ccbot

#endif

// ─── 平台无关的打包/解包 ──────────────────────────────────────────────────────
namespace ccbot {

bool KeyStore::save(const Creds& c, const std::string& path) {
    std::string plain = c.api_key + "\n" + c.api_secret + "\n" +
                        (c.testnet ? "1" : "0");
    return backend_save(plain, path);
}

bool KeyStore::load(Creds& c, const std::string& path) {
    std::string plain;
    if (!backend_load(plain, path)) return false;

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
