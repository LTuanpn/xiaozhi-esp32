"""Compile the shipped encoder and decode its actual pixels with an independent reader."""
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class QrCodeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import numpy as np
            import zxingcpp
        except ImportError:
            raise unittest.SkipTest('requires numpy and zxing-cpp for independent QR decoding')
        cls.np, cls.decoder = np, zxingcpp
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not shutil.which(compiler[0]): raise unittest.SkipTest('requires host C compiler')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        driver = Path(cls.tmp.name) / 'qr.c'
        driver.write_text('''#include <stdio.h>
#include <stdlib.h>
#include "qrcodegen.h"
int main(int argc, char **argv) {
    unsigned char qr[qrcodegen_BUFFER_LEN_MAX], temp[qrcodegen_BUFFER_LEN_MAX];
    int version = atoi(argv[2]);
    if (!qrcodegen_encodeText(argv[1], temp, qr, qrcodegen_Ecc_LOW, version, version, qrcodegen_Mask_AUTO, 1)) return 2;
    int n = qrcodegen_getSize(qr), scale = 192 / (n + 8), offset = (192 - n * scale) / 2;
    for (int y=0; y<192; y++) for (int x=0; x<192; x++) {
        int inside=x>=offset && y>=offset && x<offset+n*scale && y<offset+n*scale;
        putchar(inside && qrcodegen_getModule(qr,(x-offset)/scale,(y-offset)/scale) ? '0' : '1');
    }
    return 0;
}''', encoding='utf-8')
        cls.exe = Path(cls.tmp.name) / ('qr.exe' if os.name == 'nt' else 'qr')
        subprocess.run(compiler + ['-std=c99', '-I'+str(ROOT/'main/boards/common'), str(driver),
            str(ROOT/'main/boards/common/qrcodegen.c'), '-o', str(cls.exe)], check=True, capture_output=True)

    def test_decode_versions_and_block_boundaries(self):
        for version, text in [(1,'test'), (2,'http://192.168.4.1'), (3,'WIFI:T:WPA;S:Lab;P:12345678;;'),
                              (5,'https://example.com/'+'x'*65), (7,'https://example.com/'+'y'*100),
                              (9,'WIFI:T:WPA;S:Lab\\;A;P:abc\\:12345678;;')]:
            with self.subTest(version=version):
                pixels = subprocess.check_output([str(self.exe), text, str(version)])
                image = (self.np.frombuffer(pixels,dtype=self.np.uint8).reshape(192,192)-ord('0'))*255
                decoded = self.decoder.read_barcode(image)
                self.assertIsNotNone(decoded)
                self.assertEqual(decoded.text, text)

    def test_oversized_payload_is_rejected(self):
        result = subprocess.run([str(self.exe), 'x'*500, '9'], capture_output=True)
        self.assertEqual(result.returncode, 2)
