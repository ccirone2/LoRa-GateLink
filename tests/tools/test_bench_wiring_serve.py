"""Tests for tools/bench-wiring/serve.py: only localhost may use it, and only wiring.json is writable."""

import functools
import http.client
import http.server
import importlib.util
import os
import threading

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "bench_wiring_serve", os.path.join(HERE, "..", "..", "tools", "bench-wiring", "serve.py"))
serve = importlib.util.module_from_spec(spec)
spec.loader.exec_module(serve)


@pytest.fixture
def server(tmp_path, monkeypatch):
    (tmp_path / "wiring.json").write_text('{"wires": []}')
    monkeypatch.setattr(serve, "TARGET", str(tmp_path / "wiring.json"))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(serve.Handler, directory=str(tmp_path)))
    srv.RequestHandlerClass.log_message = lambda *a: None
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield srv.server_address[1], tmp_path
    srv.shutdown()
    srv.server_close()


def request(port, method, host, body=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    c.putrequest(method, "/wiring.json", skip_host=True)
    c.putheader("Host", host)
    if body is not None:
        c.putheader("Content-Length", str(len(body)))
    c.endheaders(body)
    r = c.getresponse()
    r.read()
    c.close()
    return r.status


@pytest.mark.parametrize("host", ["localhost:{port}", "127.0.0.1:{port}", "LOCALHOST:{port}", "localhost"])
def test_localhost_reads_and_saves(server, host):
    port, d = server
    host = host.format(port=port)
    assert request(port, "GET", host) == 200
    assert request(port, "PUT", host, b'{"wires": [1]}') == 204
    assert (d / "wiring.json").read_text() == '{"wires": [1]}'


@pytest.mark.parametrize("host", ["evil.example:{port}", "localhost.evil.example", "", "127.0.0.2:{port}"])
def test_other_hosts_are_refused(server, host):
    port, d = server
    host = host.format(port=port)
    assert request(port, "GET", host) == 403
    assert request(port, "HEAD", host) == 403
    assert request(port, "PUT", host, b'{"wires": [2]}') == 403
    assert (d / "wiring.json").read_text() == '{"wires": []}'


def test_only_valid_json_is_saved(server):
    port, d = server
    assert request(port, "PUT", f"localhost:{port}", b"{oops") == 400
    assert (d / "wiring.json").read_text() == '{"wires": []}'
