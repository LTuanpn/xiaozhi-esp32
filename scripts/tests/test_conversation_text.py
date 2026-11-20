import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


class ConversationTextTest(unittest.TestCase):
    def test_directives_duplicates_and_music_routing(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++ compiler")
        root = Path(__file__).resolve().parents[2]
        source = r'''
#include "conversation_text.h"
#include "boards/bread-compact-wifi-lcd/activity_deadline.h"
#include <cassert>

int main() {
    assert(CleanAssistantText("%emotion:happy").empty());
    assert(CleanAssistantText("  %TOOL {...}\r\nĐã bật đèn.") == "Đã bật đèn.");
    assert(CleanAssistantText("Đã bật đèn. %mcp.self.light") == "Đã bật đèn.");
    assert(CleanAssistantText("% self.screen.open_lan_control...").empty());
    assert(CleanAssistantText("Đang tìm nhạc. % play_song(...)") == "Đang tìm nhạc.");
    assert(CleanAssistantText("Âm lượng 30%.") == "Âm lượng 30%.");
    std::deque<std::string> recent;
    assert(RememberAssistantSentence(recent, "Đã bật đèn."));
    assert(RememberAssistantSentence(recent, "Đèn ở mức 30%."));
    assert(!RememberAssistantSentence(recent, "Đã bật đèn."));
    assert(!RememberAssistantSentence(recent, ""));
    recent.clear(); // New STT turn
    assert(RememberAssistantSentence(recent, "Đã bật đèn."));
    int64_t timer = 1000001, workout = timer, guard = 0;
    assert(jarvis::SecondsLeft(timer, 0) == 2);
    assert(jarvis::FirstDue(timer-1, {&timer,&workout,&guard}) == nullptr);
    auto due = jarvis::FirstDue(timer, {&timer,&workout,&guard});
    assert(due == &timer);
    assert(jarvis::FirstDue(timer+1, {&timer,&workout,&guard}) == due);
    *due = 0;
    assert(jarvis::FirstDue(workout, {&timer,&workout,&guard}) == &workout);
    assert(MusicUrlTurn("https://music.test/stream/t.ogg") == 0);
    assert(MusicUrlTurn("https://music.test/stream/t.ogg?jarvis_turn=123") == 123);
    assert(MusicUrlTurn("https://music.test/x?q=1&jarvis_turn=44&other=y") == 44);
    assert(MusicUrlTurn("https://music.test/x?jarvis_turn=8x") == -1);
    assert(IsMusicRequest("Phát bài Come My Way."));
    assert(IsMusicRequest("Jarvis, mở Come My Way của Sơn Tùng."));
    assert(!IsMusicRequest("Mở trang cấu hình của robot."));
    assert(!IsMusicRequest("Kiểm tra kết nối MCP Music."));
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source_file = Path(temporary) / "conversation.cc"
            source_file.write_text(source, encoding="utf-8")
            executable = Path(temporary) / "conversation_test"
            subprocess.run(
                compiler + ["-std=c++20", "-I", str(root / "main"), str(source_file),
                            "-o", str(executable)],
                check=True,
            )
            subprocess.run([str(executable)], check=True)
