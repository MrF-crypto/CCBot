#pragma once
#include <string>

namespace ccbot {

// 使用 Windows DPAPI 加密存储交易所 API Key/Secret
// 加密绑定当前 Windows 用户，其他账户无法解密
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
