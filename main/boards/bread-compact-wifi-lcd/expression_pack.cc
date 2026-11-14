#include "expression_pack.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include <miniz.h>

#include "board.h"

namespace {
constexpr size_t kOffset = 2 * 1024 * 1024;
constexpr size_t kSector = 4096;
constexpr size_t kPixels = ExpressionPack::kSide * ExpressionPack::kSide;
constexpr size_t kClipSize = 44;
constexpr size_t kPaletteSize = 512;
constexpr char kTag[] = "ExpressionPack";

struct Header {
    char magic[4];
    uint32_t length;
    char sha[64];
};

uint32_t Read32(const uint8_t* bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

uint16_t Read16(const uint8_t* bytes) {
    uint16_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

bool ValidImage(const uint8_t* image, size_t size) {
    if (size < 12 || memcmp(image, "REX3", 4) != 0)
        return false;
    const size_t count = Read32(image + 4);
    const size_t payload = Read32(image + 8);
    const size_t table_end = 12 + count * kClipSize;
    if (!count || count > 96 || table_end > payload || payload >= size)
        return false;
    for (size_t index = 0; index < count; ++index) {
        const uint8_t* clip = image + 12 + index * kClipSize;
        const size_t fps = Read16(clip + 32), frames = Read16(clip + 34);
        const size_t palette = Read32(clip + 36), frame_table = Read32(clip + 40);
        if (!memchr(clip, 0, 32) || !clip[0] || !fps || fps > 30 || !frames || frames > 240 ||
            palette < table_end || palette > payload || payload - palette < kPaletteSize ||
            frame_table < table_end || frame_table > payload || payload - frame_table < frames * 8)
            return false;
        for (size_t frame = 0; frame < frames; ++frame) {
            const auto* row = image + frame_table + 8 * frame;
            const size_t offset = Read32(row), length = Read32(row + 4);
            if (!length || offset >= size - payload || length > size - payload - offset)
                return false;
        }
    }
    return true;
}
}  // namespace

const esp_partition_t* ExpressionPack::Partition() {
    const auto* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (!partition || partition->size != kOffset + kReserved)
        return nullptr;
    // Older images are allowed as long as their mmap payload ends before the reserved space.
    std::array<uint32_t, 3> base{};
    if (esp_partition_read(partition, 0, base.data(), sizeof(base)) != ESP_OK || !base[0] ||
        base[2] == 0 || base[2] > kOffset - sizeof(base))
        return nullptr;
    return partition;
}

bool ExpressionPack::Download(const std::string& url, const std::string& digest) {
    const auto* partition = Partition();
    if (!partition || digest.size() != 64)
        return false;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetTimeout(8000);
    if (!http->Open("GET", url))
        return false;
    const auto status = http->GetStatusCode();
    const size_t length = http->GetBodyLength();
    if (!status || *status != 200 || length < 12 || length > kReserved - kSector)
        return false;

    using Buffer = std::unique_ptr<char, decltype(&heap_caps_free)>;
    Buffer buffer(static_cast<char*>(heap_caps_malloc(kSector, MALLOC_CAP_INTERNAL)),
                  heap_caps_free);
    if (!buffer)
        return false;
    constexpr size_t start = kOffset + kSector;
    size_t received = 0, erased = 0;
    if (esp_partition_erase_range(partition, kOffset, kSector) != ESP_OK)
        return false;
    while (received < length) {
        const auto result = http->Read(buffer.get(), std::min(length - received, kSector));
        if (!result || *result <= 0)
            return false;
        const size_t count = *result;
        while (erased < received + count) {
            if (esp_partition_erase_range(partition, start + erased, kSector) != ESP_OK)
                return false;
            erased += kSector;
        }
        if (esp_partition_write(partition, start + received, buffer.get(), count) != ESP_OK)
            return false;
        received += count;
        vTaskDelay(1);
    }
    http->Close();
    const void* mapped = nullptr;
    esp_partition_mmap_handle_t map;
    if (esp_partition_mmap(partition, start, length, ESP_PARTITION_MMAP_DATA, &mapped, &map) !=
        ESP_OK)
        return false;
    unsigned char hash[32];
    char hex[65];
    const bool valid = ValidImage(static_cast<const uint8_t*>(mapped), length) &&
                       mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                  static_cast<const unsigned char*>(mapped), length, hash) == 0;
    esp_partition_munmap(map);
    if (!valid)
        return false;
    for (size_t i = 0; i < sizeof(hash); ++i)
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    if (digest != hex)
        return false;
    Header header{{'E', 'X', 'P', '1'}, static_cast<uint32_t>(length), {}};
    memcpy(header.sha, digest.data(), sizeof(header.sha));
    return esp_partition_write(partition, kOffset, &header, sizeof(header)) == ESP_OK;
}

ExpressionPack::~ExpressionPack() {
    Reset();
    heap_caps_free(pixels_);
    heap_caps_free(indices_);
    heap_caps_free(delta_);
    heap_caps_free(inflater_);
}

void ExpressionPack::Reset() {
    Hide();
    if (map_)
        esp_partition_munmap(map_);
    map_ = 0;
    image_ = nullptr;
    size_ = 0;
    digest_.clear();
}

void ExpressionPack::Load() {
    Reset();
    const auto* partition = Partition();
    Header header{};
    const void* mapped = nullptr;
    if (!partition || esp_partition_read(partition, kOffset, &header, sizeof(header)) != ESP_OK ||
        memcmp(header.magic, "EXP1", 4) != 0 || header.length < 12 ||
        header.length > kReserved - kSector ||
        esp_partition_mmap(partition, kOffset + kSector, header.length, ESP_PARTITION_MMAP_DATA,
                           &mapped, &map_) != ESP_OK)
        return;
    image_ = static_cast<const uint8_t*>(mapped);
    size_ = header.length;
    if (!ValidImage(image_, size_)) {
        Reset();
        return;
    }
    digest_.assign(header.sha, sizeof(header.sha));
    ESP_LOGI(kTag, "Loaded expression assets");
}

void ExpressionPack::Hide() {
    if (canvas_)
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    clip_ = nullptr;
}

void ExpressionPack::Draw(lv_obj_t* parent, std::string_view name) {
    if (!image_ || name.empty()) {
        Hide();
        return;
    }
    if (!clip_ || name != reinterpret_cast<const char*>(clip_)) {
        Hide();
        for (size_t i = 0; i < Read32(image_ + 4); ++i) {
            const uint8_t* candidate = image_ + 12 + i * kClipSize;
            if (name == reinterpret_cast<const char*>(candidate)) {
                clip_ = candidate;
                break;
            }
        }
        if (!clip_)
            return;
        start_us_ = esp_timer_get_time();
        frame_ = -1;
    }
    if (!canvas_) {
        if (!indices_)
            indices_ = static_cast<uint8_t*>(heap_caps_malloc(kPixels, MALLOC_CAP_SPIRAM));
        if (!delta_)
            delta_ = static_cast<uint8_t*>(heap_caps_malloc(kPixels, MALLOC_CAP_SPIRAM));
        if (!pixels_)
            pixels_ = static_cast<uint16_t*>(heap_caps_malloc(2 * kPixels, MALLOC_CAP_SPIRAM));
        if (!inflater_)
            inflater_ = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
        if (!indices_ || !delta_ || !pixels_ || !inflater_)
            return;
        canvas_ = lv_canvas_create(parent);
        lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
        lv_canvas_set_buffer(canvas_, pixels_, kSide, kSide, LV_COLOR_FORMAT_RGB565);
    }
    const int count = Read16(clip_ + 34);
    const int64_t elapsed = (esp_timer_get_time() - start_us_) * Read16(clip_ + 32) / 1000000;
    const int target = elapsed % count;
    if (elapsed >= count || frame_ > target) {
        start_us_ = esp_timer_get_time() - target * 1000000LL / Read16(clip_ + 32);
        frame_ = -1;
    }
    if (frame_ == target)
        return;
    if (frame_ < 0)
        memset(indices_, 0, kPixels);
    const auto* palette = image_ + Read32(clip_ + 36);
    const auto* frames = image_ + Read32(clip_ + 40);
    const auto* payload = image_ + Read32(image_ + 8);
    // Decode at most two frames per LVGL tick to bound work after a stalled refresh.
    for (int step = 0; frame_ < target && step < 2; ++step) {
        const auto* row = frames + 8 * ++frame_;
        auto* inflater = static_cast<tinfl_decompressor*>(inflater_);
        size_t input_size = Read32(row + 4), output_size = kPixels;
        tinfl_init(inflater);
        if (tinfl_decompress(
                inflater, payload + Read32(row), &input_size, delta_, delta_, &output_size,
                TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) !=
                TINFL_STATUS_DONE ||
            output_size != kPixels) {
            Reset();
            return;
        }
        for (size_t i = 0; i < kPixels; ++i)
            indices_[i] ^= delta_[i];
    }
    for (size_t i = 0; i < kPixels; ++i) {
        const auto* rgb = palette + 2 * indices_[i];
        pixels_[i] = (rgb[0] << 8) | rgb[1];
    }
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(canvas_);
}
