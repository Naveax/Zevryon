#!/usr/bin/env python3
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading

from browser_competitor_webdriver import (
    WebDriverProtocolError,
    WebDriverSession,
    request_json,
    wait_for_tcp,
    webdriver_status,
)


CALLS: list[tuple[str, str, object]] = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format: str, *args) -> None:
        pass

    def _body(self) -> object:
        length = int(self.headers.get("Content-Length", "0"))
        if length == 0:
            return None
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def _send(self, value: object, status: int = 200) -> None:
        payload = json.dumps({"value": value}).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:
        body = self._body()
        CALLS.append(("GET", self.path, body))
        if self.path == "/status":
            self._send({"ready": True, "message": "ready"})
            return
        self._send(None)

    def do_POST(self) -> None:
        body = self._body()
        CALLS.append(("POST", self.path, body))
        if self.path == "/session":
            self._send(
                {
                    "sessionId": "session-1",
                    "capabilities": {"browserName": "test-browser", "browserVersion": "1"},
                }
            )
            return
        if self.path == "/session/session-1/window/rect":
            self._send({"x": 0, "y": 0, "width": body["width"], "height": body["height"]})
            return
        if self.path == "/session/session-1/execute/sync":
            self._send({"kind": "sync", "echo": body})
            return
        if self.path == "/session/session-1/execute/async":
            self._send({"kind": "async", "echo": body})
            return
        if self.path == "/protocol-error":
            self._send({"error": "invalid argument", "message": "bad test input"}, status=400)
            return
        self._send(None)

    def do_DELETE(self) -> None:
        body = self._body()
        CALLS.append(("DELETE", self.path, body))
        self._send(None)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_raises(exc_type, callable_, message: str) -> None:
    try:
        callable_()
    except exc_type:
        return
    raise AssertionError(message)


def main() -> int:
    CALLS.clear()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = int(server.server_address[1])
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{port}"
    try:
        wait_for_tcp("127.0.0.1", port, timeout_seconds=2.0, interval_seconds=0.01)
        status = webdriver_status(base_url, timeout_seconds=2.0)
        require(status == {"ready": True, "message": "ready"}, "status response drifted")

        session = WebDriverSession.create(
            base_url,
            always_match={"pageLoadStrategy": "normal"},
            timeout_seconds=2.0,
        )
        require(session.session_id == "session-1", "session ID drifted")
        require(session.capabilities["browserName"] == "test-browser", "capabilities lost")
        session.set_timeouts(script_ms=3000, page_load_ms=5000)
        rect = session.set_window_rect(width=800, height=720)
        require(rect["width"] == 800 and rect["height"] == 720, "window rect response drifted")
        session.navigate("about:blank")
        sync_result = session.execute_sync("return arguments[0]", [{"x": 1}])
        require(
            sync_result
            == {
                "kind": "sync",
                "echo": {
                    "script": "return arguments[0]",
                    "args": [{"x": 1}],
                },
            },
            "execute/sync response drifted",
        )
        async_result = session.execute_async("arguments[arguments.length - 1](arguments[0])", [7])
        require(
            async_result
            == {
                "kind": "async",
                "echo": {
                    "script": "arguments[arguments.length - 1](arguments[0])",
                    "args": [7],
                },
            },
            "execute/async response drifted",
        )
        session.close()
        session.close()

        require(
            CALLS
            == [
                ("GET", "/status", None),
                (
                    "POST",
                    "/session",
                    {"capabilities": {"alwaysMatch": {"pageLoadStrategy": "normal"}}},
                ),
                (
                    "POST",
                    "/session/session-1/timeouts",
                    {"script": 3000, "pageLoad": 5000},
                ),
                (
                    "POST",
                    "/session/session-1/window/rect",
                    {"width": 800, "height": 720},
                ),
                ("POST", "/session/session-1/url", {"url": "about:blank"}),
                (
                    "POST",
                    "/session/session-1/execute/sync",
                    {"script": "return arguments[0]", "args": [{"x": 1}]},
                ),
                (
                    "POST",
                    "/session/session-1/execute/async",
                    {
                        "script": "arguments[arguments.length - 1](arguments[0])",
                        "args": [7],
                    },
                ),
                ("DELETE", "/session/session-1", None),
            ],
            "WebDriver request sequence drifted",
        )

        require_raises(
            WebDriverProtocolError,
            lambda: request_json(
                base_url,
                "POST",
                "/protocol-error",
                {"test": True},
                timeout_seconds=2.0,
            ),
            "W3C protocol error was accepted",
        )
        require_raises(ValueError, lambda: session.execute_sync(""), "blank sync script was accepted")
        require_raises(ValueError, lambda: session.execute_async(""), "blank async script was accepted")
        require_raises(ValueError, lambda: session.navigate(""), "blank navigation URL was accepted")
        require_raises(
            ValueError,
            lambda: session.set_window_rect(width=0, height=720),
            "invalid window width was accepted",
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2.0)

    require_raises(
        ValueError,
        lambda: wait_for_tcp("127.0.0.1", 0, timeout_seconds=1.0),
        "invalid TCP port was accepted",
    )
    print("Zevryon WebDriver transport tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
