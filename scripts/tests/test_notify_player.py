"""Exercise the real notification worker/demuxer with deterministic task/HTTP fakes."""
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class NotifyPlayerTest(unittest.TestCase):
    def test_replacement_cancellation_and_drain(self):
        files = {
            'freertos/FreeRTOS.h': '''#pragma once
using UBaseType_t = unsigned; using BaseType_t = int; using TaskHandle_t = void*;
constexpr int pdPASS = 1;
''',
            'freertos/task.h': '''#pragma once
#include "FreeRTOS.h"
inline void (*pending_task)(void*) = nullptr;
inline void* pending_arg = nullptr;
inline int xTaskCreate(void (*fn)(void*), const char*, unsigned, void* arg, unsigned, void** h) {
    pending_task = fn; pending_arg = arg; *h = arg; return pdPASS;
}
inline void vTaskDelete(void*) {}
''',
            'esp_log.h': '#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n',
            'audio_service.h': '''#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
struct AudioStreamPacket {
    int sample_rate, frame_duration; uint32_t playback_id, media_position_ms;
    std::vector<uint8_t> payload;
};
class AudioService {
public:
    uint32_t generation = 1;
    std::vector<uint32_t> received;
    std::function<void()> before_push;
    uint32_t GetPlaybackGeneration() { return generation; }
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> p, bool,
                                 std::optional<uint32_t> expected = {}) {
        if (before_push) { auto fn = std::move(before_push); before_push = {}; fn(); }
        if (expected && *expected != generation) return false;
        received.push_back(p->playback_id); return true;
    }
};
''',
            'http.h': '''#pragma once
#include <functional>
#include <optional>
#include <string>
inline std::function<void()> during_read;
class Http {
public:
    void SetTimeout(int) {} void SetHeader(const char*, const char*) {}
    bool Open(const char*, const std::string&) { return true; }
    std::optional<int> GetStatusCode() { return 200; }
    std::optional<int> Read(char*, size_t) {
        if (during_read) { auto fn = std::move(during_read); during_read = {}; fn(); }
        return 0;
    }
    void Close() {}
};
''',
            'board.h': '''#pragma once
#include <memory>
#include "http.h"
struct Network { std::unique_ptr<Http> CreateHttp(int) { return std::make_unique<Http>(); } };
class Board {
public:
    static Board& GetInstance() { static Board b; return b; }
    Network* GetNetwork() { static Network n; return &n; }
};
''',
            'driver.cc': r'''#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>
#include "notify_player.h"
#include "http.h"
static void RunWorker() {
    assert(pending_task); auto fn = pending_task; pending_task = nullptr; fn(pending_arg);
}
int main(int argc, char** argv) {
    std::ifstream input(argv[1], std::ios::binary);
    const std::string ogg((std::istreambuf_iterator<char>(input)), {});
    assert(!ogg.empty());
    AudioService audio;
    NotifyPlayer player(audio);
    int finishes = 0;
    auto done = [&](uint32_t id, bool ok) { assert(ok); ++finishes; };
    assert(!player.Start("ftp://invalid", {}, 1, {}, done));
    assert(player.Start({}, {}, 1, {}, done, ogg));
    assert(!player.Start({}, {}, 2, {}, done, ogg));
    RunWorker();
    assert(player.IsActive(1)); assert(finishes == 0); assert(audio.received.size() > 10);
    player.OnPlaybackDrained(); player.OnPlaybackDrained();
    assert(finishes == 1); assert(!player.IsBusy());

    // Old HTTP worker is still unwinding when a local alarm replaces it.
    audio.received.clear();
    assert(player.Start("https://music.test/old.ogg", {}, 3, {}, done));
    during_read = [&] {
        player.Stop(); ++audio.generation;
        assert(!player.IsActive()); assert(player.IsBusy());
        assert(player.Start({}, {}, 4, {}, done, ogg));
        assert(!pending_task); // Reuse bounded worker, do not spawn more workers.
    };
    RunWorker();
    assert(!audio.received.empty());
    assert(std::all_of(audio.received.begin(), audio.received.end(), [](auto id) { return id == 4; }));
    assert(finishes == 1);
    player.OnPlaybackDrained(); assert(finishes == 2);

    // Cancellation after packet construction but before queue insertion.
    audio.received.clear();
    assert(player.Start({}, {}, 5, {}, done, ogg));
    audio.before_push = [&] { player.Stop(); ++audio.generation; };
    RunWorker();
    assert(audio.received.empty()); assert(!player.IsBusy());
    player.OnPlaybackDrained(); assert(finishes == 2);
    assert(player.Start({}, {}, 6, {}, done, ogg));
    RunWorker(); player.OnPlaybackDrained(); assert(finishes == 3);
}
'''
        }
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            for name, content in files.items():
                path = temp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding='utf-8')
            executable = temp / ('notify_test.exe' if os.name == 'nt' else 'notify_test')
            command = shlex.split(os.environ.get('CXX', 'c++')) + [
                '-std=c++20', '-pthread', f'-I{temp}', f'-I{ROOT / "main/notify"}',
                f'-I{ROOT / "main/audio/demuxer"}', str(temp / 'driver.cc'),
                str(ROOT / 'main/notify/notify_player.cc'),
                str(ROOT / 'main/audio/demuxer/ogg_demuxer.cc'), '-o', str(executable)]
            subprocess.run(command, check=True, capture_output=True, timeout=180)
            for asset in sorted(p.name for p in (ROOT / 'main/assets/common').glob('*_vi.ogg')):
                with self.subTest(asset=asset):
                    subprocess.run([str(executable), str(ROOT / 'main/assets/common' / asset)],
                                   check=True, capture_output=True, timeout=15)
