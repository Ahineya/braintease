#!/usr/bin/env python3
"""Check the existing server using the same stdio transport as the IDEA client."""

import io
import json
from pathlib import Path
import subprocess
import sys


def main():
    binary = Path(sys.argv[1]).resolve()
    uri = (Path(__file__).resolve().parent / "smoke.rfl").as_uri()
    source = "fn answer(x: i16) -> i16 { x + 1 }\nfn main() -> i16 { let value = answer(41); value }\n"
    messages = []

    def send(method, params, id=None):
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if id is not None:
            message["id"] = id
        body = json.dumps(message).encode()
        messages.append(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)

    doc = {"uri": uri}
    position = {"line": 1, "character": source.splitlines()[1].index("answer") + 1}
    send("initialize", {"processId": None, "rootUri": Path.cwd().as_uri(), "capabilities": {}}, 1)
    send("initialized", {})
    send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
    send("textDocument/hover", {"textDocument": doc, "position": position}, 2)
    send("textDocument/completion", {"textDocument": doc, "position": position}, 3)
    send("textDocument/definition", {"textDocument": doc, "position": position}, 4)
    send("textDocument/references", {"textDocument": doc, "position": position, "context": {"includeDeclaration": True}}, 5)
    send("textDocument/rename", {"textDocument": doc, "position": position, "newName": "increment"}, 6)
    send("textDocument/documentSymbol", {"textDocument": doc}, 7)
    send("textDocument/semanticTokens/full", {"textDocument": doc}, 8)
    send("textDocument/inlayHint", {"textDocument": doc, "range": {"start": {"line": 0, "character": 0}, "end": {"line": 2, "character": 0}}}, 9)
    send("textDocument/signatureHelp", {"textDocument": doc, "position": {"line": 1, "character": source.splitlines()[1].index("41")}}, 10)
    # IDEA sends full-document changes because the server advertises sync kind 1.
    send("textDocument/didChange", {"textDocument": {**doc, "version": 2}, "contentChanges": [{"text": "fn main() -> i16 { missing_name }"}]})
    send("textDocument/didChange", {"textDocument": {**doc, "version": 3}, "contentChanges": [{"text": source}]})
    send("textDocument/didClose", {"textDocument": doc})
    send("shutdown", None, 11)
    send("exit", None)
    result = subprocess.run([str(binary), "lsp"], input=b"".join(messages), capture_output=True, timeout=30, check=True)
    stream = io.BytesIO(result.stdout)
    responses, diagnostics = {}, []
    while header := stream.readline():
        assert header.lower().startswith(b"content-length:"), header
        length = int(header.split(b":", 1)[1])
        assert stream.readline() == b"\r\n"
        message = json.loads(stream.read(length))
        assert "error" not in message, message
        if "id" in message:
            responses[message["id"]] = message["result"]
        elif message.get("method") == "textDocument/publishDiagnostics":
            diagnostics.append(message["params"]["diagnostics"])
    assert responses[1]["capabilities"]["textDocumentSync"] == 1
    assert "answer" in responses[2]["contents"]["value"] or "fn(" in responses[2]["contents"]["value"]
    assert any(item["label"] == "answer" for item in responses[3]), responses[3]
    assert responses[4]["range"]["start"]["line"] == 0
    assert responses[5]
    assert len(responses[6]["changes"][uri]) >= 2
    assert {item["name"] for item in responses[7]} >= {"answer", "main"}
    assert responses[8]["data"]
    assert responses[9]
    assert responses[10]["signatures"]
    assert responses[11] is None
    assert len(diagnostics) == 3 and not diagnostics[0] and diagnostics[1] and not diagnostics[2], diagnostics
    print("LSP smoke passed: initialize, diagnostics/change recovery, hover, completion, navigation, references, rename, symbols, tokens, inlays, signatures, shutdown.")


if __name__ == "__main__":
    main()
