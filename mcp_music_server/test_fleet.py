import hashlib
import tempfile
import unittest
from unittest.mock import AsyncMock, MagicMock, patch

from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer

from fleet import Fleet
from utility_tools import TOOLS, run
from collections import Counter


class FleetTest(unittest.IsolatedAsyncioTestCase):
    def test_utility_catalog(self):
        self.assertEqual(sorted(Counter(tool["category"] for tool, _ in TOOLS.values()).values()), [5] * 10)
        samples = {"a": 9, "b": 3, "value": 10, "day": "2026-11-12", "values": [1, 3, 5],
                   "count": 3, "text": "Hello"}
        for name, (tool, _) in TOOLS.items():
            args = {key: samples[key] for key in tool["inputSchema"]["required"]}
            if name.startswith("json_"):
                args["text"] = '{"a": 1}'
            elif name == "encoding_from_base64":
                args["text"] = "aGk="
            elif name.startswith("net_cidr"):
                args["text"] = "192.0.2.1/24"
            elif name.startswith("net_ip"):
                args["text"] = "192.0.2.1"
            elif name == "net_url_host":
                args["text"] = "https://example.test"
            with self.subTest(name=name):
                self.assertIsNotNone(run(name, args))
                with self.assertRaises(ValueError):
                    run(name, {})

    async def test_assignment_targets_one_robot_and_confirms_running_image(self):
        variant = "bread-compact-wifi-lcd-240x240"
        image = bytearray(256)
        image[0] = 0xe9
        image[12] = 9
        image[32:36] = b"\x32\x54\xcd\xab"
        image[48:53] = b"2.5.4"
        image[80:87] = b"xiaozhi"
        image[176:208] = b"\x12" * 32
        image[208:208 + len(variant)] = variant.encode()
        image = bytes(image)
        cloud = web.Application()

        async def upstream(request):
            self.assertEqual(request.headers["Device-Id"], "test-mac")
            return web.json_response({"mqtt": {"endpoint": "example.test"}})

        cloud.router.add_post("/", upstream)
        with tempfile.TemporaryDirectory() as directory:
            async with TestServer(cloud) as upstream_server:
                proxy, handler = MagicMock(port=8765), MagicMock()
                proxy.public_status.return_value = {"status": "idle"}
                handler.get_tools_list.return_value = [{"name": "play_song"}]
                fleet = Fleet(directory, proxy, handler, str(upstream_server.make_url("/")))
                app = web.Application()
                fleet.attach(app)
                fleet.admin_port = 8766
                fleet.releases = [{"tag": "v2.5.4", "preview": True, "url": "https://github.com/",
                    "assets": [{"id": 42, "name": variant + ".bin", "size": len(image),
                                "url": f"https://github.com/{fleet.repository}/releases/download/v2.5.4/{variant}.bin",
                                "digest": "sha256:" + hashlib.sha256(image).hexdigest()}]}]
                ids = ["aaaaaaaa-0000-0000-0000-000000000001", "aaaaaaaa-0000-0000-0000-000000000002"]

                def payload(i, version="2.5.3", digest="01" * 32):
                    return {"uuid": ids[i], "chip_model_name": "esp32s3",
                            "board": {"name": variant}, "display": {"width": 240, "height": 240},
                            "application": {"version": version, "elf_sha256": digest},
                            "partition_table": [{"label": label, "size": 1024} for label in ("ota_0", "ota_1")]}

                async with TestClient(TestServer(app)) as robot, TestClient(TestServer(fleet.admin_app())) as admin:
                    ota = "/fleet/ota/" + fleet.key
                    device_headers = {"Device-Id": "test-mac"}
                    admin_headers = {"Host": "127.0.0.1:8766", "Origin": "http://127.0.0.1:8766"}
                    for i in range(2):
                        response = await robot.post(ota, json=payload(i), headers=device_headers)
                        self.assertEqual((await response.json())["mqtt"]["endpoint"], "example.test")
                    response = await robot.post("/fleet/ota/wrong-key", json=payload(0))
                    self.assertEqual(response.status, 404)
                    assignment = {"device": ids[0], "asset": 42}
                    response = await admin.post("/api/assignments", json=assignment,
                                                headers={"Host": "127.0.0.1:8766"})
                    self.assertEqual(response.status, 403)
                    download = MagicMock()
                    download.content.readexactly = AsyncMock(return_value=image)
                    download.content.read = AsyncMock(return_value=b"")
                    session = MagicMock()
                    session.__aenter__.return_value = session
                    session.get.return_value.__aenter__.return_value = download
                    with patch("fleet.ClientSession", return_value=session):
                        response = await admin.post("/api/assignments", json=assignment, headers=admin_headers)
                        self.assertEqual(response.status, 200, await response.text())
                    response = await robot.post(ota, json=payload(1), headers=device_headers)
                    self.assertNotIn("firmware", await response.json())
                    response = await robot.post(ota, json=payload(0), headers=device_headers)
                    self.assertEqual((await response.json())["firmware"]["version"], "2.5.4")
                    response = await robot.get(ota + "/image/42.bin")
                    self.assertEqual(await response.read(), image)
                    response = await robot.post(ota, json=payload(0, "2.5.4", "12" * 32), headers=device_headers)
                    self.assertNotIn("firmware", await response.json())
                    response = await admin.get("/api/fleet", headers=admin_headers)
                    states = {d["id"]: d["state"] for d in (await response.json())["devices"]}
                    self.assertEqual(states, {ids[0]: "installed", ids[1]: "unassigned"})
                    response = await admin.post("/api/devices/" + ids[0] + "/name",
                                                json={"name": " Robot bàn làm việc "}, headers=admin_headers)
                    self.assertEqual(response.status, 200)
                    fleet.record(payload(0, "2.5.4", "12" * 32), source="usb")
                    response = await admin.get("/api/fleet", headers=admin_headers)
                    renamed = next(row for row in (await response.json())["devices"] if row["id"] == ids[0])
                    self.assertEqual(renamed["name"], "Robot bàn làm việc")
                    self.assertIsNotNone(renamed["ota_seen"])
                fleet.db.close()


if __name__ == "__main__":
    unittest.main()
