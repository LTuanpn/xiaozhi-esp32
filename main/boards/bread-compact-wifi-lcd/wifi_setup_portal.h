#pragma once

#include <esp_http_server.h>
#include <esp_netif.h>
#include <lwip/udp.h>

class WifiSetupPortal {
public:
    bool Start(esp_ip4_addr_t gateway);
    void Stop();

private:
    static void AnswerDns(void* arg, udp_pcb* socket, pbuf* packet, const ip_addr_t* peer,
                          uint16_t port);
    static esp_err_t Handle(httpd_req_t* request);

    esp_ip4_addr_t gateway_{};
    udp_pcb* dns_ = nullptr;
    httpd_handle_t http_ = nullptr;
};
