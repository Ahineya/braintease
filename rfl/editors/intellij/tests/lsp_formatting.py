#!/usr/bin/env python3
"""Exercise formatting over the editor's real stdio protocol and the shared CLI."""

import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1]).resolve())
    uri = "file:///unsaved-formatting.rfl"
    source = "// heading\r\nfn main(){/* 🙂 */let x=1+2;println(x);}\r\n"
    expected = "// heading\r\nfn main() {\r\n    /* 🙂 */ let x = 1 + 2;\r\n    println(x);\r\n}\r\n"
    messages = []

    def send(method, params, request_id=None):
        body = {"jsonrpc": "2.0", "method": method, "params": params}
        if request_id is not None:
            body["id"] = request_id
        data = json.dumps(body).encode()
        messages.append(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)

    doc = {"uri": uri}
    options = {"tabSize": 8, "insertSpaces": False}
    send("initialize", {"capabilities": {}}, 1)
    send("initialized", {})
    send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
    send("textDocument/formatting", {"textDocument": doc, "options": options}, 2)
    # The preceding emoji occupies two UTF-16 units. Keep all edits in this selection.
    start = len(source.splitlines()[1].split("let x")[0].encode("utf-16-le")) // 2
    end = start + len("let x=1+2;")
    selection = {"start": {"line": 1, "character": start}, "end": {"line": 1, "character": end}}
    send("textDocument/rangeFormatting", {"textDocument": doc, "range": selection, "options": options}, 3)
    send("textDocument/didChange", {"textDocument": {**doc, "version": 2}, "contentChanges": [{"text": "fn main() { let x = ; }"}]})
    send("textDocument/formatting", {"textDocument": doc, "options": options}, 4)
    send("textDocument/didChange", {"textDocument": {**doc, "version": 3}, "contentChanges": [{"text": expected}]})
    send("textDocument/formatting", {"textDocument": doc, "options": options}, 5)
    http_source = (Path(__file__).resolve().parents[3] / "examples/http-server.rfl").read_text()
    http_source = http_source.replace("let request = ", "let request=").replace("let response = ", "let response=")
    send("textDocument/didChange", {"textDocument": {**doc, "version": 4}, "contentChanges": [{"text": http_source}]})
    send("textDocument/formatting", {"textDocument": doc, "options": options}, 7)
    generic_expected = "impl HttpServer {\n    export fn accept(&self) -> Async<Result<IncomingRequest, NetError>> {\n        accept(retain(pointer(&self.state)), self.epoch)\n    }\n}\n"
    generic_source = generic_expected.replace("IncomingRequest, NetError", "IncomingRequest,\n    NetError")
    send("textDocument/didChange", {"textDocument": {**doc, "version": 5}, "contentChanges": [{"text": generic_source}]})
    send("textDocument/formatting", {"textDocument": doc, "options": options}, 8)
    send("shutdown", {}, 6)
    send("exit", {})
    process = subprocess.run([binary, "lsp"], input=b"".join(messages), capture_output=True, timeout=30, check=True)
    stream = io.BytesIO(process.stdout)
    responses = {}
    while line := stream.readline():
        assert line.startswith(b"Content-Length: "), line
        size = int(line.split(b":", 1)[1])
        assert stream.readline() == b"\r\n"
        message = json.loads(stream.read(size))
        if "id" in message:
            assert "error" not in message, message
            responses[message["id"]] = message["result"]

    capabilities = responses[1]["capabilities"]
    assert capabilities["documentFormattingProvider"]
    assert capabilities["documentRangeFormattingProvider"]

    def offset(position, text):
        lines = text.splitlines(keepends=True)
        before = sum(map(len, lines[:position["line"]]))
        units = lines[position["line"]].encode("utf-16-le")[:position["character"] * 2]
        return before + len(units.decode("utf-16-le"))

    def apply(edits, text=source):
        result = text
        for edit in reversed(edits):
            a, b = offset(edit["range"]["start"], text), offset(edit["range"]["end"], text)
            assert text[a:b].isspace() or a == b
            result = result[:a] + edit["newText"] + result[b:]
        return result

    assert apply(responses[2]) == expected, apply(responses[2])
    assert apply(responses[3]) == source.replace("let x=1+2;", " let x = 1 + 2;")
    assert responses[4] == []
    assert responses[5] == []
    http_formatted = apply(responses[7], http_source)
    assert "let request = server.accept().await?;" in http_formatted
    assert "let response = if (request.method != \"GET\") {" in http_formatted
    assert apply(responses[8], generic_source) == generic_expected

    def cli(args, text=None):
        return subprocess.run([binary, "fmt", *args], input=text, capture_output=True, timeout=30)

    assert cli([], source.encode()).stdout == expected.encode()
    assert cli(["--check"], source.encode()).returncode == 1
    assert cli(["--check"], expected.encode()).returncode == 0
    with tempfile.TemporaryDirectory(prefix="rfl-format-") as temporary:
        file = Path(temporary) / "with spaces.rfl"
        file.write_bytes(source.encode())
        assert cli(["--check", str(file)]).returncode == 1
        assert file.read_bytes() == source.encode()
        invalid = Path(temporary) / "invalid.rfl"
        invalid.write_text("fn main( {")
        assert cli([str(file), str(invalid)]).returncode != 0
        assert file.read_bytes() == source.encode(), "validate all files before writing any"
        assert cli([str(file)]).returncode == 0
        assert file.read_bytes() == expected.encode()
        assert cli(["--check", str(file)]).returncode == 0
    print("Formatting checks passed: generic arguments, HTTP server assignments, unsaved buffers, UTF-16/CRLF edits, selection boundaries, syntax refusal, idempotence and CLI.")


if __name__ == "__main__":
    main()
