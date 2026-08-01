#pragma once
#include <string>

namespace ccbot {

// 加密存储交易所 API Key/Secret，按平台选择后端：
//   Windows = DPAPI（绑定当前用户）；macOS = 系统钥匙串（Keychain）
// 密文/条目均归属当前系统用户，换电脑或换账户后需要重新输入
class KeyStore {
public:
    struct Creds {
        std::string api_key;
        std::string api_secret;
        bool        testnet = true;
    };

    static bool save(const Creds& c, const std::string& path);
    static bool load(Creds& c,       const std::string& path);
};

} // namespace ccbot
