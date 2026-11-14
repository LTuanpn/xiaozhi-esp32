#pragma once

#include <esp_partition.h>
#include <lvgl.h>

#include <cstdint>
#include <string>
#include <string_view>

class ExpressionPack {
public:
    static constexpr size_t kReserved = 6 * 1024 * 1024;
    static constexpr int kSide = 240;
    static const esp_partition_t* Partition();
    static bool Download(const std::string& url, const std::string& digest);

    ~ExpressionPack();
    void Destroy();
    void Load();
    void Reset();
    bool Draw(lv_obj_t* parent, std::string_view name);
    void Hide();
    const std::string& digest() const { return digest_; }

private:
    const uint8_t* image_ = nullptr;
    size_t size_ = 0;
    esp_partition_mmap_handle_t map_ = 0;
    std::string digest_;
    lv_obj_t* canvas_ = nullptr;
    uint16_t* pixels_ = nullptr;
    uint8_t* indices_ = nullptr;
    uint8_t* delta_ = nullptr;
    void* inflater_ = nullptr;
    const uint8_t* clip_ = nullptr;
    std::string requested_;
    int frame_ = -1;
    int64_t start_us_ = 0;
};
