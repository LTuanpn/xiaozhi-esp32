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
#include <cassert>

int main() {
    assert(CleanAssistantText("%emotion:happy").empty());
    assert(CleanAssistantText("  %TOOL {...}\r\nĐã bật đèn.") == "Đã bật đèn.");
    assert(CleanAssistantText("Đã bật đèn. %mcp.self.light") == "Đã bật đèn.");
    assert(CleanAssistantText("Âm lượng 30%.") == "Âm lượng 30%.");
    std::deque<std::string> recent;
    assert(RememberAssistantSentence(recent, "Đã bật đèn."));
    assert(RememberAssistantSentence(recent, "Đèn ở mức 30%."));
    assert(!RememberAssistantSentence(recent, "Đã bật đèn."));
    assert(!RememberAssistantSentence(recent, ""));
    recent.clear(); // New STT turn
    assert(RememberAssistantSentence(recent, "Đã bật đèn."));
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
