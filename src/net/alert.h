#pragma once
#include <string>

namespace ccbot {

// 关键事件外部提醒：往一个 webhook URL 发一条文本消息。根据 URL 自动识别平台，
// 拼不同平台各自要求的 JSON body 格式（企业微信/飞书/Telegram 三者 body 结构互不相同，
// 发错格式对方大多会返回 HTTP 200 但内容被静默拒绝，界面上完全看不出来）：
//  1) 企业微信群机器人（url 含 qyapi.weixin.qq.com）：{"msgtype":"text","text":{"content":"..."}}
//  2) 飞书自定义机器人（url 含 open.feishu.cn）：{"msg_type":"text","content":{"text":"..."}}
//  3) Telegram Bot API（url 含 api.telegram.org）：{"text":"..."}（chat_id 已经在 URL 查询参数里）
//  4) 其它：退回通用 {"text":"..."} JSON body
// 同步阻塞（内部用 curl），调用方必须放在后台线程，不能在 GUI 线程直接调用。
// err_out 非空时，失败会写入 HTTP 响应体或 curl 错误信息，方便定位具体是哪里配错了。
bool send_webhook(const std::string& url, const std::string& text, std::string* err_out = nullptr);

} // namespace ccbot
