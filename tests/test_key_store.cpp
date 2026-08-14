// KeyStore 打包格式的回归测试。
// 重点是**向后兼容**：加账户类型字段（第4行）之前保存的凭证是3行的，
// 升级后必须照常读得出来，否则用户的 Key 会"凭空消失"、还得重新去交易所翻。
#include "core/key_store.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_fail;
}

// 直接造一份"旧版格式"的密文（3行，没有账户类型），模拟升级前保存的文件
static bool write_legacy_blob(const std::string& path, const std::string& plain) {
#if defined(_WIN32)
    DATA_BLOB in  = { (DWORD)plain.size(), (BYTE*)plain.data() };
    DATA_BLOB out = {};
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    bool ok = (bool)f;
    if (ok) f.write((char*)out.pbData, (std::streamsize)out.cbData);
    LocalFree(out.pbData);
    return ok;
#else
    // 非 Windows 后端是明文文件，直接写
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(plain.data(), (std::streamsize)plain.size());
    return true;
#endif
}

int main() {
    const std::string path = "test_creds.bin";

    // ① 新格式往返：普通合约
    {
        KeyStore::Creds w;
        w.api_key = "KEY_futures"; w.api_secret = "SEC_futures";
        w.testnet = true;  w.account_mode = 0;
        check(KeyStore::save(w, path), "保存普通合约凭证");

        KeyStore::Creds r;
        check(KeyStore::load(r, path), "读回普通合约凭证");
        check(r.api_key == w.api_key,         "  api_key 一致");
        check(r.api_secret == w.api_secret,   "  api_secret 一致");
        check(r.testnet == true,              "  testnet=true 保持");
        check(r.account_mode == 0,            "  account_mode=0 保持");
    }

    // ② 新格式往返：统一账户
    {
        KeyStore::Creds w;
        w.api_key = "KEY_pm"; w.api_secret = "SEC_pm";
        w.testnet = false; w.account_mode = 1;
        check(KeyStore::save(w, path), "保存统一账户凭证");

        KeyStore::Creds r;
        check(KeyStore::load(r, path), "读回统一账户凭证");
        check(r.api_key == "KEY_pm",  "  api_key 一致");
        check(r.testnet == false,     "  testnet=false 保持");
        check(r.account_mode == 1,    "  account_mode=1 保持");
    }

    // ③ 向后兼容：升级前保存的3行密文
    {
        check(write_legacy_blob(path, "OLDKEY\nOLDSEC\n1"), "造一份旧版3行密文");

        KeyStore::Creds r;
        r.account_mode = 1;   // 先污染成非默认值，确认 load 真的会写回去
        check(KeyStore::load(r, path), "读旧版密文");
        check(r.api_key == "OLDKEY",  "  旧 api_key 正确");
        check(r.api_secret == "OLDSEC","  旧 api_secret 正确");
        check(r.testnet == true,      "  旧 testnet 标志正确");
        check(r.account_mode == 0,    "  旧密文退回普通合约账户");
    }

    // ④ 旧版 testnet=0 的分支
    {
        check(write_legacy_blob(path, "K2\nS2\n0"), "造旧版密文(testnet=0)");
        KeyStore::Creds r;
        check(KeyStore::load(r, path), "读旧版密文(testnet=0)");
        check(r.testnet == false,   "  testnet=false 正确");
        check(r.account_mode == 0,  "  account_mode=0 正确");
    }

    std::remove(path.c_str());
    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
