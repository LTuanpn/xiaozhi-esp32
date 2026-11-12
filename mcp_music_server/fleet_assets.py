"""Read-only previews and USB initiation of the firmware's assets updater."""
import asyncio
from functools import lru_cache
import json
from pathlib import Path
import re
import struct
import sys

from aiohttp import web

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.build_default_assets import read_assets_entries

VARIANT = "bread-compact-wifi-lcd-240x240"
# These two application builds shipped the same assets image.
COMPATIBLE_APPS = {
    "25a767ab5852ae66cf84bc3e7463004598901eaf7a6d48b133ae48169f14143b",
    "3130a72c125e876509fdc403902cc929a6e6d6b9cb17bbb21c7ecfce7b18565b",
}
PACK_NAMES = {"original": "Gốc", "mint": "Xanh ngọc"}
PACK_LIMIT = 8 * 1024 * 1024


def png_size(data):
    if len(data) < 24 or data[:16] != b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR":
        raise ValueError("Ảnh xem trước phải là PNG.")
    width, height = struct.unpack_from(">II", data, 16)
    if not 1 <= width <= 240 or not 1 <= height <= 240:
        raise ValueError("Kích thước PNG vượt khung 240×240.")
    return width, height


def pack(root, key):
    if key not in PACK_NAMES:
        raise ValueError("Gói giao diện không tồn tại.")
    path = root / f"{key}.bin"
    if not path.is_file() or not 0 < path.stat().st_size <= PACK_LIMIT:
        raise ValueError("Gói giao diện chưa được cài trên server.")
    original = root / "original.bin"
    if not original.is_file() or original.stat().st_size > PACK_LIMIT:
        raise ValueError("Thiếu gói gốc hoặc gói gốc vượt phân vùng.")
    return read_pack(path, original, path.stat().st_mtime_ns, original.stat().st_mtime_ns)


@lru_cache(maxsize=4)
def read_pack(path, baseline, _mtime, _baseline_mtime):
    files = read_assets_entries(path)
    index = json.loads(files["index.json"][0])
    original = read_assets_entries(baseline) if path != baseline else files
    if files.keys() != original.keys() or any(
        item != original[name] for name, item in files.items() if not name.endswith(".png")
    ):
        raise ValueError("Gói phải giữ nguyên index, font và mô hình giọng nói gốc.")
    if not {index["srmodels"], index["text_font"]} <= files.keys():
        raise ValueError("Gói thiếu font hoặc mô hình giọng nói.")
    previews = []
    for row in index["emoji_collection"]:
        name, filename = row["name"], row["file"]
        if not re.fullmatch(r"[a-z0-9_-]+\.png", filename):
            raise ValueError("Tên ảnh không hợp lệ.")
        width, height = png_size(files[filename][0])
        if (width, height) != png_size(original[filename][0]):
            raise ValueError("Gói đã thay kích thước ảnh gốc.")
        previews.append(dict(name=name, file=filename, width=width, height=height))
    return path, files, previews


class AssetRoutes:
    def __init__(self, fleet):
        self.fleet = fleet
        self.root = fleet.path / "interfaces"

    async def catalog(self, _request):
        packs = []
        for key, title in PACK_NAMES.items():
            if (self.root / f"{key}.bin").exists():
                path, _, previews = pack(self.root, key)
                packs.append(dict(id=key, title=title, bytes=path.stat().st_size, previews=previews))
        return web.json_response({"packs": packs})

    async def preview(self, request):
        try:
            _, files, previews = pack(self.root, request.match_info["pack"])
        except (ValueError, KeyError, OSError):
            raise web.HTTPNotFound() from None
        filename = request.match_info["file"]
        if not any(row["file"] == filename for row in previews):
            raise web.HTTPNotFound()
        return web.Response(body=files[filename][0], content_type="image/png")

    async def download(self, request):
        self.fleet.authorize_device(request)
        try:
            path, _, _ = pack(self.root, request.match_info["pack"])
        except (ValueError, KeyError, OSError):
            raise web.HTTPNotFound() from None
        return web.FileResponse(path)

    async def apply(self, request):
        from fleet import open_serial, serial_request

        data = await request.json()
        if (not isinstance(data, dict) or not all(isinstance(data.get(k), str) for k in ("port", "pack", "device"))):
            raise web.HTTPBadRequest(text="Chọn robot, cổng USB và gói giao diện.")
        key = data["pack"]
        try:
            path, _, _ = pack(self.root, key)
        except (ValueError, KeyError, OSError) as exc:
            raise web.HTTPBadRequest(text=str(exc)) from None
        url = f'{self.fleet.base().rstrip("/")}/fleet/ota/{self.fleet.key}/assets/{key}.bin'
        if len(url) > 240:
            raise web.HTTPBadRequest(text="URL tài nguyên quá dài.")
        size = path.stat().st_size

        def send():
            with open_serial(data["port"]) as connection:
                def call(name, arguments):
                    response = serial_request(connection, "tools/call", {"name": name, "arguments": arguments})
                    if response.get("isError"):
                        raise ValueError("Robot từ chối lệnh cập nhật tài nguyên.")
                    return response

                info = json.loads(call("self.get_system_info", {})["content"][0]["text"])
                partitions = info.get("partition_table", [])
                if (info.get("uuid", "").lower() != data["device"]
                        or info.get("board", {}).get("name") != VARIANT
                        or info.get("chip_model_name") != "esp32s3"
                        or info.get("application", {}).get("elf_sha256") not in COMPATIBLE_APPS
                        or not any(p.get("label") == "assets" and p.get("size", 0) >= size for p in partitions)):
                    raise ValueError("Cổng USB không khớp robot đã chọn hoặc phân vùng assets không phù hợp.")
                call("self.assets.set_download_url", {"url": url})
                call("self.reboot", {})

        async with self.fleet.serial_lock:
            try:
                await asyncio.to_thread(send)
            except (OSError, ValueError, KeyError, TimeoutError) as exc:
                raise web.HTTPBadRequest(text=str(exc)) from None
        return web.json_response({"status": "queued"})
