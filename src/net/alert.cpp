#include "net/alert.h"
#include <curl/curl.h>
#include <sstream>

namespace ccbot {

static std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
        case '"':  o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n";  break;
        case '\r': break;
        default:   o << c;
        }
    }
    return o.str();
}

static size_t write_to_string(char* ptr, size_t sz, size_t n, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, sz * n);
    return sz * n;
}

static bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

bool send_webhook(const std::string& url, const std::string& text, std::string* err_out) {
    if (url.empty()) return false;

    std::string esc = json_escape(text);
    std::string body;
    if (contains(url, "qyapi.weixin.qq.com")) {
        // 企业微信群机器人：必须是 {"msgtype":"text","text":{"content":"..."}}，
        // 发通用的 {"text":"..."} 格式对方会返回 200 + errcode!=0，界面上完全看不出失败
        body = "{\"msgtype\":\"text\",\"text\":{\"content\":\"" + esc + "\"}}";
    } else if (contains(url, "open.feishu.cn") || contains(url, "open.larksuite.com")) {
        // 飞书/Lark 自定义机器人
        body = "{\"msg_type\":\"text\",\"content\":{\"text\":\"" + esc + "\"}}";
    } else {
        // Telegram Bot API（chat_id 在 URL 查询参数里）、或其它接受 {"text":...} 的通用 webhook
        body = "{\"text\":\"" + esc + "\"}";
    }

    CURL* curl = curl_easy_init();
    if (!curl) { if (err_out) *err_out = "curl_easy_init 失败"; return false; }

    struct curl_slist* hdrs = curl_slist_append(nullptr, "Content-Type: application/json");

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        if (err_out) *err_out = std::string("curl错误: ") + curl_easy_strerror(rc);
        return false;
    }
    if (http_code < 200 || http_code >= 300) {
        if (err_out) *err_out = "HTTP " + std::to_string(http_code) + ": " + resp;
        return false;
    }
    // 企业微信/飞书就算 HTTP 200，body 里也可能是 {"errcode":301xxx,...} 表示业务层失败
    // （key 不对、机器人被移出群等），"errcode":0 才是真的发成功
    if ((contains(url, "qyapi.weixin.qq.com") || contains(url, "open.feishu.cn")) &&
        !contains(resp, "\"errcode\":0")) {
        if (err_out) *err_out = "接口返回: " + resp;
        return false;
    }

    return true;
}

} // namespace ccbot
