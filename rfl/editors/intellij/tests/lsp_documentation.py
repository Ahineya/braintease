#!/usr/bin/env python3
"""Documentation Markdown through the same stdio hover request IDEA sends."""
import io
import json
from pathlib import Path
import subprocess
import sys


def position(text, offset):
    before = text[:offset]
    return {"line": before.count("\n"), "character": len(before.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}


def main():
    path = Path(__file__).resolve().parents[3] / "examples/documentation-hover.rfl"
    doc = {"uri": path.as_uri()}
    # The source is intentionally never saved to disk.
    source = '''import { Request as HttpRequest } from "std/http";
import { abs as absolute, PI } from "std/math";
import { Future, ready } from "std/future";
/// Computes a **documented** value. 🙂
///
/// # Example
/// ```rfl
/// let n = documented();
/// ```
fn documented()->i16 { 42 }
fn main() {
    let request = HttpRequest::get("example.com", "/");
    let value = absolute(2);
    let circle = PI;
    let sample = documented();
    println(value);
    request.port;
    let mut future = ready(1);
    future.cancel();
}
'''
    messages = []

    def send(method, params, request_id=None):
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if request_id is not None:
            message["id"] = request_id
        body = json.dumps(message).encode()
        messages.append(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)

    send("initialize", {"capabilities": {"textDocument": {"hover": {"contentFormat": ["markdown"]}}}}, 1)
    send("initialized", {})
    send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
    checks = [
        ("HttpRequest }", 0, "plain HTTP GET request"),
        ("absolute(2)", 0, "absolute value"),
        ("HttpRequest::get", len("HttpRequest::"), "let request = Request::get(\"example.com\", \"/\");"),
        ("= PI", 2, "circle constant"),
        ("sample = documented()", len("sample = "), "# Example\n```rfl\nlet n = documented();\n```"),
        ("println(value)", 0, "followed by a newline"),
        ("request.port", len("request."), "TCP port; defaults to 80"),
        ("future.cancel", len("future."), "Synchronously cancels unfinished work"),
    ]
    for request_id, (needle, delta, _) in enumerate(checks, 2):
        send("textDocument/hover", {"textDocument": doc, "position": position(source, source.index(needle) + delta)}, request_id)
    changed = source.replace("Computes a **documented** value. 🙂", "Updated **unsaved** documentation. 🙂")
    send("textDocument/didChange", {"textDocument": {**doc, "version": 2}, "contentChanges": [{"text": changed}]})
    send("textDocument/hover", {"textDocument": doc, "position": position(changed, changed.index("sample = documented()") + len("sample = "))}, 20)
    send("shutdown", {}, 21)
    send("exit", {})
    result = subprocess.run([str(Path(sys.argv[1]).resolve()), "lsp"], input=b"".join(messages), capture_output=True, check=True, timeout=60)
    responses, stream = {}, io.BytesIO(result.stdout)
    while header := stream.readline():
        size = int(header.split(b":", 1)[1])
        assert stream.readline() == b"\r\n"
        message = json.loads(stream.read(size))
        assert "error" not in message, message
        if message.get("method") == "textDocument/publishDiagnostics":
            assert message["params"]["diagnostics"] == [], message
        if "id" in message:
            responses[message["id"]] = message["result"]
    assert responses[1]["capabilities"]["hoverProvider"]
    for request_id, (needle, _, expected) in enumerate(checks, 2):
        contents = responses[request_id]["contents"]
        assert contents["kind"] == "markdown", contents
        assert contents["value"].startswith("```rfl\n"), contents
        assert expected in contents["value"], (needle, contents)
        assert "$" not in contents["value"], contents
    assert "Updated **unsaved** documentation. 🙂" in responses[20]["contents"]["value"]
    assert "Computes a **documented** value" not in responses[20]["contents"]["value"]
    print("Documentation checks passed: Markdown, examples, unsaved updates, imported aliases, associated functions, constants, fields and implicit standard-library printing.")


if __name__ == "__main__":
    main()
