#include "sdkconfig.h"

#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
#include "wifi_setup_portal.h"

#include <esp_timer.h>
#include <lwip/sockets.h>
#include <ssid_manager.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include "application.h"
#include "mcp_server.h"

namespace {
constexpr char kPage[] = R"HTML(<!doctype html>
<html lang="vi"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Cấu hình Wi-Fi Xiaozhi</title>
<style>body{font:1rem system-ui,sans-serif;max-width:28rem;margin:2rem auto;padding:0 1rem;color:#182230}form{display:grid;gap:.7rem}input,button{box-sizing:border-box;width:100%;padding:.7rem;font:inherit}button{background:#1f5ca8;color:white;border:0;border-radius:.3rem}button:disabled{opacity:.65}</style></head>
<body><h1>Cấu hình Wi-Fi Xiaozhi</h1><p>Nhập Wi-Fi robot sẽ dùng sau khi khởi động lại.</p>
<form><label for="ssid">Tên mạng Wi-Fi</label><input id="ssid" maxlength="32" required autocapitalize="none" spellcheck="false">
<label for="password">Mật khẩu Wi-Fi</label><input id="password" type="password" maxlength="64" autocomplete="new-password">
<button>Lưu và khởi động lại</button></form><p id="status" role="status"></p>
<script>
const form=document.querySelector('form'), button=form.querySelector('button'), status=document.querySelector('#status');
form.onsubmit=async event=>{
  event.preventDefault(); button.disabled=true; status.textContent='Đang lưu…';
  try {
    const reply=await fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:form.querySelector('#ssid').value,password:form.querySelector('#password').value})});
    status.textContent=await reply.text(); button.disabled=reply.ok;
  } catch { status.textContent='Mất kết nối với Xiaozhi. Hãy kiểm tra Wi-Fi của điện thoại.'; button.disabled=false; }
};
</script></body></html>)HTML";
}  // namespace

bool WifiSetupPortal::Start(esp_ip4_addr_t gateway) {
    gateway_ = gateway;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.ctrl_port = 32771;
    config.max_open_sockets = 2;
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 1;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.global_user_ctx = this;
    config.open_fn = [](httpd_handle_t server, int fd) {
        auto* portal = static_cast<WifiSetupPortal*>(httpd_get_global_user_ctx(server));
        sockaddr_storage local{};
        socklen_t size = sizeof(local);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &size) != 0)
            return ESP_FAIL;
        if (local.ss_family == AF_INET) {
            const auto& address = reinterpret_cast<const sockaddr_in&>(local).sin_addr;
            return address.s_addr == portal->gateway_.addr ? ESP_OK : ESP_FAIL;
        }
#if CONFIG_LWIP_IPV6
        if (local.ss_family == AF_INET6) {
            const auto& address = reinterpret_cast<const sockaddr_in6&>(local).sin6_addr;
            if (IN6_IS_ADDR_V4MAPPED(&address) &&
                memcmp(address.s6_addr + 12, &portal->gateway_.addr,
                       sizeof(portal->gateway_.addr)) == 0)
                return ESP_OK;
        }
#endif
        return ESP_FAIL;
    };
    if (httpd_start(&http_, &config) != ESP_OK)
        return false;
    const httpd_uri_t page{.uri = "/*", .method = HTTP_GET, .handler = Handle, .user_ctx = this};
    const httpd_uri_t save{
        .uri = "/wifi", .method = HTTP_POST, .handler = Handle, .user_ctx = this};
    if (httpd_register_uri_handler(http_, &page) == ESP_OK &&
        httpd_register_uri_handler(http_, &save) == ESP_OK &&
        esp_netif_tcpip_exec(
            [](void* arg) -> esp_err_t {
                auto* portal = static_cast<WifiSetupPortal*>(arg);
                auto* socket = udp_new_ip_type(IPADDR_TYPE_V4);
                if (!socket)
                    return ESP_ERR_NO_MEM;
                ip_addr_t address;
                ip_addr_set_ip4_u32(&address, portal->gateway_.addr);
                if (udp_bind(socket, &address, 53) != ERR_OK) {
                    udp_remove(socket);
                    return ESP_FAIL;
                }
                portal->dns_ = socket;
                udp_recv(socket, AnswerDns, portal);
                return ESP_OK;
            },
            this) == ESP_OK) {
        return true;
    }
    Stop();
    return false;
}

void WifiSetupPortal::Stop() {
    if (http_) {
        httpd_stop(http_);
        http_ = nullptr;
    }
    esp_netif_tcpip_exec(
        [](void* arg) -> esp_err_t {
            auto* portal = static_cast<WifiSetupPortal*>(arg);
            if (portal->dns_) {
                udp_remove(portal->dns_);
                portal->dns_ = nullptr;
            }
            return ESP_OK;
        },
        this);
}

void WifiSetupPortal::AnswerDns(void* arg, udp_pcb* socket, pbuf* packet, const ip_addr_t* peer,
                                uint16_t port) {
    // A captive portal only needs to answer one uncompressed IN question.
    std::array<uint8_t, 512> reply{};
    const size_t bytes = pbuf_copy_partial(packet, reply.data(), reply.size() - 16, 0);
    const ip_addr_t client = *peer;
    pbuf_free(packet);
    if (bytes < 17 || (reply[2] & 0xf8) || reply[4] != 0 || reply[5] != 1)
        return;
    size_t end = 12;
    while (end < bytes && reply[end] != 0) {
        if (reply[end] > 63 || end + reply[end] + 1 >= bytes)
            return;
        end += reply[end] + 1;
    }
    if (end > 267 || end + 5 > bytes || reply[end + 3] != 0 || reply[end + 4] != 1)
        return;
    const bool ipv4 = reply[end + 1] == 0 && reply[end + 2] == 1;
    end += 5;
    reply[2] |= 0x84;
    reply[3] = 0;
    std::fill(reply.begin() + 6, reply.begin() + 12, 0);
    if (ipv4) {
        reply[7] = 1;
        constexpr uint8_t record[]{0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 15, 0, 4};
        memcpy(reply.data() + end, record, sizeof(record));
        end += sizeof(record);
        auto* portal = static_cast<WifiSetupPortal*>(arg);
        memcpy(reply.data() + end, &portal->gateway_.addr, sizeof(portal->gateway_.addr));
        end += sizeof(portal->gateway_.addr);
    }
    if (auto* response = pbuf_alloc(PBUF_TRANSPORT, end, PBUF_RAM)) {
        if (pbuf_take(response, reply.data(), end) == ERR_OK)
            udp_sendto(socket, response, &client, port);
        pbuf_free(response);
    }
}

esp_err_t WifiSetupPortal::Handle(httpd_req_t* request) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (request->method == HTTP_GET) {
        if (std::string_view(request->uri) != "/") {
            auto* portal = static_cast<WifiSetupPortal*>(request->user_ctx);
            char location[24];
            snprintf(location, sizeof(location), "http://" IPSTR "/", IP2STR(&portal->gateway_));
            httpd_resp_set_status(request, "303 See Other");
            httpd_resp_set_hdr(request, "Location", location);
            return httpd_resp_sendstr(request, "Mở trang cấu hình Xiaozhi");
        }
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        return httpd_resp_sendstr(request, kPage);
    }

    char type[32]{};
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") != 0 || request->content_len > 768) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Dữ liệu Wi-Fi không hợp lệ.");
    }
    std::string body(request->content_len, '\0');
    const int64_t deadline = esp_timer_get_time() + 1000000;
    for (size_t offset = 0; offset < body.size();) {
        if (esp_timer_get_time() >= deadline)
            return ESP_FAIL;
        const int size = httpd_req_recv(request, body.data() + offset, body.size() - offset);
        if (size <= 0)
            return ESP_FAIL;
        offset += size;
    }
    CJsonUniquePtr data(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, nullptr, true));
    const auto* ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data.get(), "ssid"));
    const auto* password =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data.get(), "password"));
    if (!ssid || !*ssid || strlen(ssid) > 32 || !password || strlen(password) > 64 ||
        (*password && strlen(password) < 8)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Kiểm tra tên và mật khẩu Wi-Fi.");
    }
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    const auto sent = httpd_resp_sendstr(request, "Đã nhận Wi-Fi. Robot đang khởi động lại.");
    if (sent == ESP_OK) {
        Application::GetInstance().Schedule(
            [ssid = std::string(ssid), key = std::string(password)]() {
                SsidManager::GetInstance().AddSsid(ssid, key);
                Application::GetInstance().Reboot();
            });
    }
    return sent;
}
#endif
