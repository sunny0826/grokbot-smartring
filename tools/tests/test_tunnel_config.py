import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("tunnel_config", Path(__file__).parents[1] / "configure_tunnel.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class TunnelRoutesTest(unittest.TestCase):
    def setUp(self):
        self.config = {"tunnel": "existing-tunnel", "credentials-file": "/private/credentials.json",
                       "ingress": [{"hostname": "ring.example.com", "service": "http://127.0.0.1:8766"},
                                   {"service": "http_status:404"}]}

    def update(self, config=None):
        return module.device_routes(config or self.config, "ring.example.com",
                                    "https://192.168.1.10:8768", Path("/private/ca.pem"))

    def test_original_mcp_and_other_settings_survive(self):
        result = self.update()
        self.assertEqual(result["ingress"][2:], self.config["ingress"])
        self.assertEqual(result["tunnel"], self.config["tunnel"])
        self.assertEqual(result["credentials-file"], self.config["credentials-file"])
        self.assertEqual(len(self.config["ingress"]), 2)
        self.assertEqual(result["ingress"][0]["service"], "http://127.0.0.1:8770")
        self.assertEqual(result["ingress"][1]["originRequest"], {
            "caPool": "/private/ca.pem", "originServerName": "192.168.1.10"})

    def test_repeat_is_idempotent(self):
        first = self.update()
        self.assertEqual(self.update(first), first)

    def test_existing_different_device_route_requires_review(self):
        changed = self.update()
        changed["ingress"][0]["service"] = "http://127.0.0.1:9999"
        with self.assertRaisesRegex(ValueError, "differs"):
            self.update(changed)

    def test_no_new_hostname_or_unknown_mcp_target(self):
        self.config["ingress"][0]["service"] = "http://127.0.0.1:9999"
        with self.assertRaisesRegex(ValueError, "original hostname"):
            self.update()

    def test_public_or_plaintext_voice_origin_is_rejected(self):
        for origin in ("http://192.168.1.10:8768", "https://8.8.8.8:8768",
                       "https://user:password@192.168.1.10:8768", "https://192.168.1.10:8768/extra"):
            with self.subTest(origin=origin), self.assertRaises(ValueError):
                module.device_routes(self.config, "ring.example.com", origin, Path("/private/ca.pem"))


if __name__ == "__main__":
    unittest.main()
