"""用回环上游验证示例服务器的 Jev 转发，两个参数分别为服务器程序和 MiniLM 模型目录。"""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.request import Request, urlopen


class Provider(BaseHTTPRequestHandler):
    def do_POST(self):
        received = self.rfile.read(int(self.headers["Content-Length"]))
        self.server.received.append((self.path, self.headers.get("Authorization"), received))
        self.send_response(429 if len(self.server.received) == 2 else 200)
        self.send_header("Content-Type", "application/json")
        if len(self.server.received) == 2:
            self.send_header("Retry-After", "3")
        self.end_headers()
        self.wfile.write(b'{"provider":"response"}')

    def log_message(self, *_):
        pass


def main(executable, model_dir):
    root = Path(__file__).resolve().parents[2]
    config = json.loads((root / "config.example.json").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory() as directory, ThreadingHTTPServer(("127.0.0.1", 0), Provider) as provider:
        provider.received = []
        threading.Thread(target=provider.serve_forever, daemon=True).start()
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        config["persona_gateway"].update(address="127.0.0.1", port=port)
        config["persona_gateway"]["static_files"]["enabled"] = False
        config["gateway_auth"].update(enabled=True, require_auth_for_api=True, allow_dev_identity=False,
                                      enable_dev_registration=True, session_store_backend="sqlite",
                                      session_database_path=str(Path(directory, "auth.sqlite")))
        for section in ("skills", "l0_memory", "l3_memory", "l3_flush_scheduler", "document_store",
                        "document_llm_chunk_cache", "document_semantic_cache", "emotion_analyzer", "emotion_fusion"):
            config[section]["enabled"] = False
        config["embedding"].update(model_path=str(model_dir / "model.onnx"), tokenizer_path=str(model_dir / "tokenizer.json"),
                                   execution_provider="cpu")
        config["logging"]["log_dir"] = directory
        config["llm"]["enable_dev_routes"] = True
        config["llm"]["openai"].update(enabled=False, allow_placeholder=True)
        config["llm"]["jev"].update(endpoint=f"http://127.0.0.1:{provider.server_port}/v1/systemone",
                                    ca_bundle_path="", api_key_env="JEV_PROXY_FIXTURE_KEY")
        path = Path(directory, "gateway.json")
        path.write_text(json.dumps(config), encoding="utf-8")
        env = dict(os.environ, JEV_PROXY_FIXTURE_KEY="provider-test-key")
        with subprocess.Popen([executable, path, "--config", path, "--bert", model_dir / "model.onnx"], cwd=directory, env=env,
                              stdin=subprocess.PIPE, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)) as server:
            try:
                deadline = time.monotonic() + 30
                while True:
                    if server.poll() is not None:
                        raise RuntimeError("示例服务器启动失败")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=1):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.1)
                registration = Request(f"http://127.0.0.1:{port}/api/auth/register", data=b'{"userUuid":"proxy-test"}',
                                       headers={"Content-Type": "application/json"})
                with urlopen(registration, timeout=5) as response:
                    cookie = response.headers["Set-Cookie"].split(";", 1)[0]
                body = b'{ "state":"example", "model":"test", "questions":{"q":{"type":"choice","criteria":{"a":"yes","b":"no"}}} }'
                for status in (200, 429):
                    request = Request(f"http://127.0.0.1:{port}/v1/systemone", data=body,
                                      headers={"Content-Type": "application/json", "Cookie": cookie})
                    try:
                        response = urlopen(request, timeout=5)
                    except HTTPError as error:
                        response = error
                    with response:
                        assert response.status == status, response.status
                        if status == 429:
                            assert response.headers["Retry-After"] == "3"
                        assert response.read() == b'{"provider":"response"}'
                assert provider.received == [("/v1/systemone", "Bearer provider-test-key", body)] * 2
            finally:
                if server.poll() is None:
                    server.stdin.write(b"\n")
                    server.stdin.flush()
                    try:
                        server.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server.kill()
                provider.shutdown()


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
