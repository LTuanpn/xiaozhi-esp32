#include "lan_control.h"

#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string_view>

#include "application.h"
#include "mcp_server.h"

extern const char page_start[] asm("_binary_lan_control_html_start");
extern const char page_end[] asm("_binary_lan_control_html_end");
extern const char worklet_start[] asm("_binary_lan_mic_js_start");
extern const char worklet_end[] asm("_binary_lan_mic_js_end");

namespace {
constexpr int kPort = 8080;
constexpr size_t kFrameBytes = 16000 * OPUS_FRAME_DURATION_MS / 1000 * sizeof(int16_t);

std::string Token() {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    char hex[33];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        snprintf(hex + 2 * i, 3, "%02x", static_cast<unsigned>(bytes[i]));
    return hex;
}

esp_err_t Send(httpd_req_t* req, const char* status, std::string_view body,
               const char* type = "application/json") {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self' 'unsafe-inline'; "
                       "style-src 'self' 'unsafe-inline'; frame-ancestors 'none'");
    return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t Error(httpd_req_t* req, const char* status, const char* message) {
    CJsonUniquePtr json(cJSON_CreateObject());
    if (json)
        cJSON_AddStringToObject(json.get(), "error", message);
    CJsonStringUniquePtr text(json ? cJSON_PrintUnformatted(json.get()) : nullptr);
    return Send(req, status, text ? text.get() : "{\"error\":\"Unavailable\"}");
}

bool ReadBody(httpd_req_t* req, std::string& body, size_t limit) {
    if (req->content_len > limit)
        return false;
    body.resize(req->content_len);
    size_t offset = 0;
    const auto deadline = esp_timer_get_time() + 2000000;
    while (offset < body.size()) {
        if (esp_timer_get_time() >= deadline)
            return false;
        const auto count = httpd_req_recv(req, body.data() + offset, body.size() - offset);
        if (count <= 0)
            return false;
        offset += count;
    }
    return true;
}

struct Reply {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    std::atomic_bool expired{false};
    std::string body;
    ~Reply() {
        if (done)
            vSemaphoreDelete(done);
    }

    void Finish(std::string result) {
        body = std::move(result);
        xSemaphoreGive(done);
    }
};

bool Allowed(std::string_view name) {
    static constexpr std::string_view tools[] = {
        "self.audio_speaker.set_volume", "self.screen.set_brightness", "self.timer.set_countdown",
        "self.security.guard_mode",      "self.screen.set_emotion",    "self.screen.set_standby",
        "self.screen.show_qr",           "self.set_ota_url",           "self.reboot",
        "self.lan.start_listening",      "self.lan.set_https_origin",
#ifdef CONFIG_BREAD_COMPACT_LCD_KCX
        "self.bluetooth.status", "self.bluetooth.power", "self.bluetooth.device", "self.bluetooth.route",
#endif
    };
    return std::find(std::begin(tools), std::end(tools), name) != std::end(tools);
}
}  // namespace

std::string LanControl::Pair(const std::string& ip, const std::string& origin, Status status) {
    if (!server_) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = kPort;
        config.ctrl_port = 32770;
        config.max_open_sockets = 3;
        config.stack_size = 6144;
        config.recv_wait_timeout = 2;
        config.send_wait_timeout = 2;
        config.lru_purge_enable = true;
        config.uri_match_fn = httpd_uri_match_wildcard;
        status_ = std::move(status);
        if (httpd_start(&server_, &config) != ESP_OK)
            return {};
        httpd_uri_t route{};
        route.uri = "/*";
        route.handler = Handle;
        route.user_ctx = this;
        for (auto method : {HTTP_GET, HTTP_POST}) {
            route.method = method;
            if (httpd_register_uri_handler(server_, &route) != ESP_OK) {
                httpd_stop(server_);
                server_ = nullptr;
                return {};
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pairing_ = Token();
    pair_until_ = esp_timer_get_time() + 120000000;
    const auto base = origin.empty() ? "http://" + ip + ":" + std::to_string(kPort) : origin;
    return base + "/#pair=" + pairing_;
}

esp_err_t LanControl::Handle(httpd_req_t* request) {
    return static_cast<LanControl*>(request->user_ctx)->HandleRequest(request);
}

esp_err_t LanControl::HandleRequest(httpd_req_t* req) {
    const std::string_view uri(req->uri);
    if (req->method == HTTP_GET && uri == "/")
        return Send(req, "200 OK", {page_start, static_cast<size_t>(page_end - page_start)},
                    "text/html; charset=utf-8");
    if (req->method == HTTP_GET && uri == "/lan_mic.js")
        return Send(req, "200 OK",
                    {worklet_start, static_cast<size_t>(worklet_end - worklet_start)},
                    "text/javascript");

    char auth[64]{};
    const auto now = esp_timer_get_time();
    if (req->method == HTTP_POST && uri == "/api/pair") {
        std::string token;
        if (!ReadBody(req, token, 32))
            return Error(req, "400 Bad Request", "Mã ghép cặp không hợp lệ.");
        std::string session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto paired_at = esp_timer_get_time();
            if (!pairing_.empty() && paired_at < pair_until_ && token == pairing_) {
                pairing_.clear();
                session = session_ = Token();
                session_until_ = paired_at + 3600000000LL;
            }
        }
        if (session.empty())
            return Error(req, "403 Forbidden", "Mở lại QR trên robot để ghép cặp.");
        return Send(req, "200 OK", "{\"session\":\"" + session + "\"}");
    }
    bool authorized;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        authorized =
            !session_.empty() && now < session_until_ &&
            httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) == ESP_OK &&
            auth == "Bearer " + session_;
    }
    if (!authorized)
        return Error(req, "401 Unauthorized", "Phiên đã hết hạn. Quét QR mới.");

    auto& app = Application::GetInstance();
    static std::weak_ptr<Reply> pending;
    if (!pending.expired())
        return Error(req, "503 Service Unavailable", "Robot đang xử lý một thao tác khác.");
    auto reply = std::make_shared<Reply>();
    pending = reply;
    if (!reply->done)
        return Error(req, "503 Service Unavailable", "Robot đang bận.");

    if (req->method == HTTP_GET && uri == "/api/status") {
        app.Schedule([this, reply]() {
            if (reply->expired)
                return;
            CJsonUniquePtr status(status_());
            CJsonStringUniquePtr text(status ? cJSON_PrintUnformatted(status.get()) : nullptr);
            reply->Finish(text ? text.get() : "{}");
        });
    } else if (req->method == HTTP_POST && uri == "/api/tool") {
        std::string body;
        if (!ReadBody(req, body, 1536))
            return Error(req, "400 Bad Request", "Yêu cầu quá dài hoặc không đầy đủ.");
        CJsonUniquePtr call(
            cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, nullptr, true));
        const auto* name = cJSON_GetObjectItemCaseSensitive(call.get(), "name");
        const auto* args = cJSON_GetObjectItemCaseSensitive(call.get(), "arguments");
        if (!cJSON_IsString(name) || !Allowed(name->valuestring) || !cJSON_IsObject(args))
            return Error(req, "400 Bad Request", "Thao tác không được phép.");
        const std::string_view tool(name->valuestring);
        CJsonUniquePtr params(cJSON_CreateObject());
        // The LAN Wi-Fi button can only open the existing setup portal.
        CJsonUniquePtr arguments(tool == "self.screen.show_qr" ? cJSON_CreateObject()
                                                               : cJSON_Duplicate(args, true));
        CJsonUniquePtr rpc(cJSON_CreateObject());
        if (!rpc || !params || !arguments ||
            !cJSON_AddStringToObject(params.get(), "name", name->valuestring))
            return Error(req, "503 Service Unavailable", "Robot đang bận.");
        cJSON_AddItemToObject(params.get(), "arguments", arguments.release());
        cJSON_AddStringToObject(rpc.get(), "jsonrpc", "2.0");
        cJSON_AddNumberToObject(rpc.get(), "id", 1);
        cJSON_AddStringToObject(rpc.get(), "method", "tools/call");
        cJSON_AddItemToObject(rpc.get(), "params", params.release());
        McpServer::GetInstance().ParseMessage(
            rpc.get(), [reply](const std::string& value) { reply->Finish(value); });
    } else if (req->method == HTTP_POST && uri.starts_with("/api/phone/")) {
        char raw_id[12]{};
        if (httpd_req_get_hdr_value_str(req, "X-Phone-Turn", raw_id, sizeof(raw_id)) != ESP_OK)
            return Error(req, "400 Bad Request", "Thiếu mã lượt nói.");
        uint32_t id = 0;
        const auto [end, ec] = std::from_chars(raw_id, raw_id + strlen(raw_id), id);
        if (ec != std::errc{} || *end || !id)
            return Error(req, "400 Bad Request", "Mã lượt nói không hợp lệ.");

        const std::string action(uri.substr(strlen("/api/phone/")));
        std::vector<int16_t> pcm;
        if (action == "audio") {
            std::string body;
            if (!ReadBody(req, body, 2 * kFrameBytes) || body.empty() ||
                body.size() % kFrameBytes != 0)
                return Error(req, "400 Bad Request", "Khung PCM không hợp lệ.");
            pcm.resize(body.size() / sizeof(int16_t));
            memcpy(pcm.data(), body.data(), body.size());
        } else if (req->content_len || (action != "begin" && action != "ready" && action != "end" &&
                                        action != "cancel")) {
            return Error(req, "400 Bad Request", "Thao tác mic không hợp lệ.");
        }
        if (action == "ready")
            return Send(req, "200 OK",
                        app.IsPhoneTalkReady(id) ? "{\"ok\":true}" : "{\"ok\":false}");
        app.Schedule([this, reply, action, id, pcm = std::move(pcm)]() mutable {
            if (reply->expired)
                return;
            auto& app = Application::GetInstance();
            bool ok = false;
            if (action == "begin") {
                CJsonUniquePtr state(status_());
                ok = state &&
                     !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state.get(), "guard")) &&
                     app.BeginPhoneTalk(id);
            } else if (action == "audio")
                ok = app.FeedPhoneTalk(id, std::move(pcm));
            else if (action == "end")
                ok = app.EndPhoneTalk(id);
            else
                ok = app.CancelPhoneTalk(id);
            reply->Finish(ok ? "{\"ok\":true}" : "{\"ok\":false}");
        });
    } else {
        return Error(req, "404 Not Found", "Không có trang này.");
    }

    if (xSemaphoreTake(reply->done, pdMS_TO_TICKS(1500)) != pdTRUE) {
        reply->expired = true;
        return Error(req, "504 Gateway Timeout",
                     "Robot chưa phản hồi. Kiểm tra trạng thái trước khi thử lại.");
    }
    return Send(req, "200 OK", reply->body);
}
