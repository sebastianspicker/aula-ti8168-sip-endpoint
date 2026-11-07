"""build-payload.py must validate its configs before it creates the output tree."""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1] / "payload/build-payload.py"
REPO = SOURCE.parents[2]


def load():
    spec = importlib.util.spec_from_file_location("build_payload_under_test", SOURCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BuildPayloadTest(unittest.TestCase):
    def build(self, nginx_text, gateway_value):
        module = load()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp).resolve()
            work = tmp / ".work"
            work.mkdir()
            args = {}
            for name in ("sipd", "gateway", "device", "nginx", "atomic", "mime", "fcgi", "sipconf"):
                path = tmp / name
                path.write_text("x")
                path.chmod(0o755)
                args[name] = path
            ui = tmp / "ui"
            ui.mkdir()
            (ui / "index.html").write_text("x")
            nginx = tmp / "nginx.conf"
            nginx.write_text(nginx_text)
            gateway = tmp / "gateway.conf"
            gateway.write_text(json.dumps(gateway_value))
            output = work / "out"
            argv = ["build-payload.py", "--sipd", str(args["sipd"]), "--gateway", str(args["gateway"]),
                    "--device", str(args["device"]), "--nginx", str(args["nginx"]),
                    "--atomic-replace", str(args["atomic"]), "--ui-dist", str(ui),
                    "--nginx-config", str(nginx), "--mime-types", str(args["mime"]),
                    "--fastcgi-params", str(args["fcgi"]), "--sip-config", str(args["sipconf"]),
                    "--gateway-config", str(gateway), "--output", str(output)]
            with mock.patch.object(module, "WORK_ROOT", work), mock.patch.object(sys, "argv", argv):
                try:
                    module.main()
                    error = None
                except SystemExit as exit_error:
                    error = str(exit_error)
            return error, output.exists(), (work / "payload-manifest.sha256").exists()

    def test_missing_nginx_anchor_leaves_no_output(self):
        error, output_exists, receipt_exists = self.build("server { listen 8443 ssl; }\n", {})
        self.assertIn("required packaging anchor", error)
        self.assertFalse(output_exists)
        self.assertFalse(receipt_exists)

    def test_bad_gateway_schema_leaves_no_output(self):
        nginx = (REPO / "product/console/nginx/nginx.conf").read_text()
        error, output_exists, _ = self.build(nginx, {"unexpected": 1})
        self.assertIn("gateway config example has an unexpected schema", error)
        self.assertFalse(output_exists)


if __name__ == "__main__":
    unittest.main()
