"""Device check-ins, release staging and the local administration API."""
import asyncio
import hmac
from hashlib import sha256
import json
import os
import re
import secrets
import sqlite3
import time
from pathlib import Path

from aiohttp import ClientError, ClientSession, ClientTimeout, web

from config import STREAM_HOST_OVERRIDE, get_lan_ip
from fleet_assets import AssetRoutes
from utility_tools import TOOLS


VARIANTS = {"bread-compact-wifi-lcd-240x240": (240, 240),
            "bread-compact-wifi-lcd-240x320": (240, 320)}
VERSION = re.compile(r"\d{1,5}\.\d{1,5}\.\d{1,5}")
UI = Path(__file__).resolve().parents[1] / "fleet-ui" / "dist"
OTA_HEADERS = ("Device-Id", "Client-Id", "Activation-Version", "Serial-Number",
               "User-Agent", "Accept-Language", "Content-Type")


def app_description(image):
    if (len(image) < 208 or image[0] != 0xe9 or image[12:14] != b"\x09\x00"
            or image[32:36] != b"\x32\x54\xcd\xab"):
        raise ValueError("Cần ảnh ứng dụng ESP32-S3 (.bin).")
    version = image[48:80].split(b"\0", 1)[0].decode("ascii")
    project = image[80:112].split(b"\0", 1)[0]
    digest = image[176:208].hex()
    if not VERSION.fullmatch(version) or project != b"xiaozhi" or not any(image[176:208]):
        raise ValueError("Ảnh phải là xiaozhi với phiên bản dạng x.y.z.")
    return version, digest


def version_key(value):
    return tuple(map(int, value.split("."))) if VERSION.fullmatch(value) else ()


class Fleet:
    def __init__(self, directory, proxy, handler, upstream=None, repository=None):
        self.path = Path(directory)
        self.path.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(self.path / "fleet.sqlite3")
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT);
            CREATE TABLE IF NOT EXISTS devices (id TEXT PRIMARY KEY, variant TEXT, version TEXT,
                digest TEXT, slot_size INTEGER, seen REAL);
            CREATE TABLE IF NOT EXISTS assignments (id TEXT PRIMARY KEY, asset INTEGER,
                version TEXT, digest TEXT, offered REAL, variant TEXT);
        """)
        if "source" not in {column["name"] for column in self.db.execute("PRAGMA table_info(devices)")}:
            self.db.execute("ALTER TABLE devices ADD COLUMN source TEXT NOT NULL DEFAULT 'ota'")
        self.db.execute("CREATE TABLE IF NOT EXISTS device_labels (id TEXT PRIMARY KEY, name TEXT NOT NULL)")
        self.db.execute("CREATE TABLE IF NOT EXISTS ota_seen (id TEXT PRIMARY KEY, seen REAL NOT NULL)")
        self.db.execute("INSERT OR IGNORE INTO ota_seen SELECT id, seen FROM devices WHERE source='ota'")
        self.db.execute("INSERT OR IGNORE INTO settings VALUES ('ota_key', ?)", (secrets.token_urlsafe(24),))
        self.db.commit()
        self.key = self.db.execute("SELECT value FROM settings WHERE key='ota_key'").fetchone()[0]
        self.proxy, self.handler = proxy, handler
        self.upstream = (upstream or os.getenv("FLEET_UPSTREAM_OTA",
                         "https://api.tenclass.net/xiaozhi/ota/")).rstrip("/")
        self.repository = repository or os.getenv("FLEET_GITHUB_REPO", "LTuanpn/xiaozhi-esp32")
        if not re.fullmatch(r"[\w.-]+/[\w.-]+", self.repository, re.ASCII):
            raise ValueError("FLEET_GITHUB_REPO must be owner/repo")
        self.connected = False
        self.releases = []
        self.runner = None
        self.serial_lock = asyncio.Lock()
        self.assets = AssetRoutes(self)

    def attach(self, app):
        app.router.add_post("/fleet/ota/{key}", self.check)
        app.router.add_post("/fleet/ota/{key}/activate", self.check)
        app.router.add_get("/fleet/ota/{key}/image/{asset:[0-9]+}.bin", self.image)
        app.router.add_get("/fleet/ota/{key}/assets/{pack:[a-z]+}.bin", self.assets.download)

    def base(self):
        return os.getenv("FLEET_PUBLIC_URL") or f"http://{STREAM_HOST_OVERRIDE or get_lan_ip()}:{self.proxy.port}"

    def authorize_device(self, request):
        if not hmac.compare_digest(request.match_info["key"].encode(), self.key.encode()):
            raise web.HTTPNotFound()

    async def image(self, request):
        self.authorize_device(request)
        path = self.path / (request.match_info["asset"] + ".bin")
        if not path.is_file():
            raise web.HTTPNotFound()
        return web.FileResponse(path)

    async def check(self, request):
        self.authorize_device(request)
        if request.content_type != "application/json":
            raise web.HTTPUnsupportedMediaType()
        if request.content_length is None or not 0 <= request.content_length <= 32768:
            raise web.HTTPBadRequest(text="OTA body must have a length of at most 32 KiB")
        try:
            raw = await asyncio.wait_for(request.read(), 5)
        except asyncio.TimeoutError:
            raise web.HTTPRequestTimeout() from None
        activate = request.path.endswith("/activate")
        try:
            payload = json.loads(raw)
            device = None if activate else self.record(payload)
        except (TypeError, ValueError, AttributeError):
            raise web.HTTPBadRequest(text="Invalid OTA device description") from None
        headers = {name: request.headers[name] for name in OTA_HEADERS if name in request.headers}
        try:
            async with ClientSession(timeout=ClientTimeout(total=12)) as session:
                async with session.post(self.upstream + ("/activate" if activate else "/"),
                                        data=raw, headers=headers, allow_redirects=False) as response:
                    if activate or response.status != 200:
                        status = response.status if response.status in (200, 202, 400, 401, 403, 429) else 502
                        return web.Response(status=status)
                    data = await response.json(content_type=None)
        except (ClientError, asyncio.TimeoutError, ValueError):
            raise web.HTTPBadGateway(text="Xiaozhi OTA upstream unavailable") from None
        if not isinstance(data, dict):
            raise web.HTTPBadGateway()
        if device:
            assignment = self.db.execute("SELECT * FROM assignments WHERE id=?", (device["id"],)).fetchone()
            if assignment:
                # Keep the assigned device pinned even if upstream advertises another image.
                data.pop("firmware", None)
                if (device["digest"] != assignment["digest"]
                        and version_key(device["version"])
                        and version_key(assignment["version"]) > version_key(device["version"])
                        and device["variant"] == assignment["variant"]):
                    path = self.path / f'{assignment["asset"]}.bin'
                    if path.is_file() and path.stat().st_size <= device["slot_size"]:
                        data["firmware"] = {"version": assignment["version"],
                                            "url": f'{self.base().rstrip("/")}/fleet/ota/{self.key}/image/{assignment["asset"]}.bin'}
                        self.db.execute("UPDATE assignments SET offered=? WHERE id=?", (time.time(), device["id"]))
                        self.db.commit()
        return web.json_response(data)

    def record(self, payload, source="ota"):
        device_id = str(payload.get("uuid", ""))
        if not re.fullmatch(r"[0-9a-fA-F-]{36}", device_id):
            raise ValueError("Invalid device UUID")
        board, display, application = (payload.get(key, {}) for key in ("board", "display", "application"))
        variant = str(board.get("name", ""))[:80]
        if VARIANTS.get(variant) != (display.get("width"), display.get("height")) or payload.get("chip_model_name") != "esp32s3":
            variant = "Chưa hỗ trợ"
        sizes = [p["size"] for p in payload.get("partition_table", [])
                 if p.get("label") in ("ota_0", "ota_1") and type(p.get("size")) is int]
        device = dict(id=device_id.lower(), variant=variant,
                      version=str(application.get("version", ""))[:32],
                      digest=str(application.get("elf_sha256", ""))[:64],
                      slot_size=min(sizes) if len(sizes) == 2 else 0, seen=time.time(), source=source)
        self.db.execute("""INSERT OR REPLACE INTO devices
            (id, variant, version, digest, slot_size, seen, source)
            VALUES (:id,:variant,:version,:digest,:slot_size,:seen,:source)""", device)
        if source == "ota":
            self.db.execute("INSERT OR REPLACE INTO ota_seen VALUES (?, ?)", (device["id"], device["seen"]))
        self.db.commit()
        return device

    async def refresh(self, _request):
        try:
            async with ClientSession(timeout=ClientTimeout(total=15)) as session:
                async with session.get(f"https://api.github.com/repos/{self.repository}/releases?per_page=30",
                                       headers={"Accept": "application/vnd.github+json"}) as response:
                    response.raise_for_status()
                    releases = await response.json()
        except (ClientError, asyncio.TimeoutError, ValueError):
            raise web.HTTPBadGateway(text="Không đọc được GitHub Releases.") from None
        self.releases = [
            dict(tag=release["tag_name"], preview=release["prerelease"], url=release["html_url"],
                 assets=[dict(id=a["id"], name=a["name"], size=a["size"], url=a["browser_download_url"],
                              digest=a.get("digest")) for a in release["assets"]])
            for release in releases if not release["draft"]
        ]
        return await self.snapshot(_request)

    async def assign(self, request):
        try:
            data = await request.json()
            if not isinstance(data, dict) or not isinstance(data.get("device"), str) or type(data.get("asset")) is not int:
                raise ValueError("Chọn robot và file hợp lệ.")
            device = self.db.execute("SELECT * FROM devices WHERE id=?", (data["device"],)).fetchone()
            asset = next(a for r in self.releases for a in r["assets"] if a["id"] == data["asset"])
            if not device or device["variant"] not in VARIANTS:
                raise ValueError("Robot hoặc cấu hình màn hình chưa được hỗ trợ.")
            if asset["name"] != device["variant"] + ".bin":
                raise ValueError("Tên file phát hành phải khớp mẫu robot.")
            if not 208 <= asset["size"] <= device["slot_size"]:
                raise ValueError("File vượt dung lượng phân vùng OTA.")
            expected = f"https://github.com/{self.repository}/releases/download/"
            if not asset["url"].startswith(expected):
                raise ValueError("URL tải không thuộc repo đã cấu hình.")
            async with ClientSession(timeout=ClientTimeout(total=30)) as session:
                async with session.get(asset["url"]) as response:
                    response.raise_for_status()
                    image = await response.content.readexactly(asset["size"])
                    if await response.content.read(1):
                        raise ValueError("Kích thước file không khớp GitHub.")
            version, digest = app_description(image)
            if device["variant"].encode("ascii") + b"\0" not in image:
                raise ValueError("Không tìm thấy tên mẫu robot trong ảnh ứng dụng.")
            if not version_key(device["version"]) or version_key(version) <= version_key(device["version"]):
                raise ValueError("Phiên bản nhúng phải mới hơn phiên bản robot đang chạy.")
            if asset["digest"] and asset["digest"] != "sha256:" + sha256(image).hexdigest():
                raise ValueError("SHA-256 không khớp GitHub.")
        except (KeyError, StopIteration, TypeError, ValueError) as exc:
            raise web.HTTPBadRequest(text=str(exc) or "Chọn robot và file hợp lệ.") from None
        except (ClientError, asyncio.TimeoutError, asyncio.IncompleteReadError):
            raise web.HTTPBadGateway(text="Không tải đủ firmware từ GitHub.") from None
        path = self.path / f'{asset["id"]}.bin'
        pending = path.with_suffix(".partial")
        pending.write_bytes(image)
        pending.replace(path)
        self.db.execute("INSERT OR REPLACE INTO assignments VALUES (?, ?, ?, ?, NULL, ?)",
                        (device["id"], asset["id"], version, digest, device["variant"]))
        self.db.commit()
        return await self.snapshot(request)

    async def clear(self, request):
        self.db.execute("DELETE FROM assignments WHERE id=?", (request.match_info["id"],))
        self.db.commit()
        return await self.snapshot(request)

    async def snapshot(self, _request):
        devices = self.db.execute("""
            SELECT d.id, d.variant, d.version, d.seen, d.source, a.version AS target,
            l.name AS name, o.seen AS ota_seen,
            CASE WHEN a.id IS NULL THEN 'unassigned' WHEN d.digest = a.digest THEN 'installed'
                WHEN a.offered IS NULL THEN 'queued' ELSE 'offered' END AS state
            FROM devices d LEFT JOIN assignments a ON d.id=a.id
            LEFT JOIN device_labels l ON d.id=l.id LEFT JOIN ota_seen o ON d.id=o.id ORDER BY d.seen DESC
        """)
        return web.json_response(dict(devices=[dict(d) for d in devices], releases=self.releases,
            repository=self.repository, connected=self.connected, music=self.proxy.public_status(),
            tools=[dict(tool, category=TOOLS[tool["name"]][0]["category"] if tool["name"] in TOOLS else "Âm nhạc")
                   for tool in self.handler.get_tools_list()], ports=serial_ports(),
            ota_url=f'{self.base().rstrip("/")}/fleet/ota/{self.key}'))

    async def rename(self, request):
        device_id = request.match_info["id"]
        data = await request.json()
        name = data.get("name") if isinstance(data, dict) else None
        if not isinstance(name, str) or not 1 <= len(name.strip()) <= 60 or not name.strip().isprintable():
            raise web.HTTPBadRequest(text="Tên robot cần từ 1 đến 60 ký tự hiển thị.")
        if not self.db.execute("SELECT 1 FROM devices WHERE id=?", (device_id,)).fetchone():
            raise web.HTTPNotFound(text="Robot không tồn tại.")
        self.db.execute("INSERT OR REPLACE INTO device_labels VALUES (?, ?)", (device_id, name.strip()))
        self.db.commit()
        return await self.snapshot(request)

    async def inspect_usb(self, request):
        data = await request.json()
        port = data.get("port") if isinstance(data, dict) else None
        if not isinstance(port, str):
            raise web.HTTPBadRequest(text="Cần tên cổng USB Serial.")
        async with self.serial_lock:
            try:
                tools, info = await asyncio.to_thread(serial_inventory, port)
                device = self.record(info, source="usb")
            except (ImportError, OSError, ValueError, TimeoutError) as exc:
                raise web.HTTPBadRequest(text=str(exc)) from None
        return web.json_response({"tools": tools, "device": device["id"]})

    def admin_app(self):
        @web.middleware
        async def local_only(request, handler):
            if request.host not in {f"127.0.0.1:{self.admin_port}", f"localhost:{self.admin_port}"}:
                raise web.HTTPForbidden()
            if request.method in {"POST", "DELETE"}:
                if request.headers.get("Origin") != f"http://{request.host}":
                    raise web.HTTPForbidden()
            response = await handler(request)
            response.headers["Cache-Control"] = "no-store"
            return response

        async def index(_request):
            if not (UI / "index.html").is_file():
                raise web.HTTPServiceUnavailable(text="Build dashboard: cd fleet-ui && npm ci && npm run build")
            return web.FileResponse(UI / "index.html")

        app = web.Application(middlewares=[local_only], client_max_size=8192)
        app.router.add_get("/", index)
        app.router.add_get("/api/fleet", self.snapshot)
        app.router.add_post("/api/releases", self.refresh)
        app.router.add_post("/api/assignments", self.assign)
        app.router.add_post("/api/usb/tools", self.inspect_usb)
        app.router.add_post("/api/devices/{id}/name", self.rename)
        app.router.add_get("/api/interfaces", self.assets.catalog)
        app.router.add_get("/api/interfaces/{pack}/{file}", self.assets.preview)
        app.router.add_post("/api/interfaces/apply", self.assets.apply)
        app.router.add_delete("/api/assignments/{id}", self.clear)
        if (UI / "assets").is_dir():
            app.router.add_static("/assets", UI / "assets")
        return app

    async def start(self, port):
        self.admin_port = port
        self.runner = web.AppRunner(self.admin_app(), access_log=None)
        await self.runner.setup()
        await web.TCPSite(self.runner, "127.0.0.1", port).start()

    async def stop_admin(self):
        if self.runner:
            await self.runner.cleanup()


def serial_ports():
    try:
        from serial.tools.list_ports import comports
    except ImportError:
        return []
    return [{"device": entry.device, "description": entry.description}
            for entry in sorted(comports())]


def open_serial(port):
    import serial

    port = port.strip()
    match = re.fullmatch(r"COM\s*(\d+)", port, re.IGNORECASE | re.ASCII)
    if match:
        port = "COM" + match[1]
    if port not in {entry["device"] for entry in serial_ports()}:
        raise ValueError(f"Server không thấy cổng {port}. Kiểm tra cáp USB và chọn lại cổng.")
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=1)
    connection.port = port
    connection.dtr = connection.rts = False
    return connection


def serial_request(connection, method, params):
    rid = secrets.randbelow(2**30)
    message = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    line = ("\n" + message + "\n").encode()
    for offset in range(0, len(line), 16):
        connection.write(line[offset:offset + 16])
        time.sleep(.015)
    deadline = time.monotonic() + 4
    buffer = b""
    while time.monotonic() < deadline:
        chunk = connection.readline(16384)
        buffer = (buffer + chunk)[-16384:]
        if not chunk.endswith(b"\n"):
            continue
        line, buffer = buffer, b""
        if b"MCPCLI " not in line:
            continue
        try:
            response = json.loads(line.split(b"MCPCLI ", 1)[1])
        except ValueError:
            continue
        if response.get("id") == rid:
            if "error" in response:
                raise ValueError("Robot từ chối yêu cầu MCP.")
            return response["result"]
    raise TimeoutError("Robot chưa trả lời MCP qua USB.")


def serial_inventory(port):
    tools, cursor = [], ""
    with open_serial(port) as connection:
        for _ in range(12):
            result = serial_request(connection, "tools/list", {"withUserTools": True, "cursor": cursor})
            tools.extend(result["tools"])
            cursor = result.get("nextCursor", "")
            if not cursor:
                info = serial_request(connection, "tools/call",
                                      {"name": "self.get_system_info", "arguments": {}})
                return tools, json.loads(info["content"][0]["text"])
    raise ValueError("Danh sách tool vượt giới hạn phân trang.")
