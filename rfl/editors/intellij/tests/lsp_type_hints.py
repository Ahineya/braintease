#!/usr/bin/env python3
"""Source-level type names through the actual LSP transport used by IDEA."""
import io
import json
from pathlib import Path
import re
import subprocess
import sys


def position(text, offset):
    before = text[:offset]
    return {"line": before.count("\n"), "character": len(before.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}


def main():
    path = Path(__file__).resolve().parents[3] / "examples/http-server.rfl"
    source = path.read_text()
    doc = {"uri": path.as_uri()}
    messages = []

    def send(method, params, request_id=None):
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if request_id is not None:
            message["id"] = request_id
        body = json.dumps(message).encode()
        messages.append(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)

    send("initialize", {"capabilities": {}}, 1)
    send("initialized", {})
    send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
    send("textDocument/inlayHint", {"textDocument": doc, "range": {
        "start": position(source, 0), "end": position(source, len(source))}}, 2)
    network = re.search(r"\blet\s+(network)\s*=", source)
    send("textDocument/hover", {"textDocument": doc, "position": position(source, network.start(1))}, 3)
    send("textDocument/signatureHelp", {"textDocument": doc, "position": position(source, source.index("serve(move(") + len("serve("))}, 4)
    send("textDocument/completion", {"textDocument": doc, "position": position(source, source.index("network.reactor") + len("network."))}, 5)
    send("shutdown", {}, 6)
    send("exit", {})
    result = subprocess.run([str(Path(sys.argv[1]).resolve()), "lsp"], input=b"".join(messages), capture_output=True, check=True, timeout=60)
    stream, responses = io.BytesIO(result.stdout), {}
    while header := stream.readline():
        size = int(header.split(b":", 1)[1])
        assert stream.readline() == b"\r\n"
        message = json.loads(stream.read(size))
        assert "error" not in message, message
        if "id" in message:
            responses[message["id"]] = message["result"]
    hints = {(h["position"]["line"], h["position"]["character"]): h["label"] for h in responses[2]}
    for name, expected in [
        ("network", ": Network"), ("reactor", ": NetworkReactor"),
        ("lease", ": Async<Result<NetworkConfig, NetError>>"),
        ("config", ": NetworkConfig"), ("server", ": HttpServer"),
        ("service", ": Async<Result<unit, NetError>>"),
    ]:
        binding = re.search(r"\blet\s+(?:mut\s+)?(" + name + r")\s*=", source)
        at = position(source, binding.end(1))
        assert hints[(at["line"], at["character"])] == expected, (name, hints)
    assert "network: Network" in responses[3]["contents"]["value"], responses[3]
    assert "HttpServer" in responses[4]["signatures"][0]["label"], responses[4]
    assert any(item["label"] == "reactor" for item in responses[5]), responses[5]
    for request in (2, 3, 4, 5):
        assert "$" not in json.dumps(responses[request]), (request, responses[request])
    print("Type display checks passed: HTTP server inlays, nested generics, hover, signature help and member completion use source-level names.")


if __name__ == "__main__":
    main()
