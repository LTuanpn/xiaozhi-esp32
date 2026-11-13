#pragma once

#include <esp_http_server.h>
#include <cJSON.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class LanControl {
public:
    using Status = std::function<cJSON*()>;
    std::string Pair(const std::string& ip, const std::string& origin, Status status);

private:
    static esp_err_t Handle(httpd_req_t* request);
    esp_err_t HandleRequest(httpd_req_t* request);
    httpd_handle_t server_ = nullptr;
    std::mutex mutex_;
    std::string pairing_;
    std::string session_;
    int64_t pair_until_ = 0;
    int64_t session_until_ = 0;
    Status status_;
};
