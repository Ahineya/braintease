#!/usr/bin/env python3
"""Constructor kinds through the same compiler/LSP transport used by IDEA."""
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def position(source, at):
    before = source[:at]
    return {"line": before.count("\n"), "character": len(before.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="rfl-kinds-") as directory:
        root = Path(directory).resolve()
        (root / "library.rfl").write_text(
            'export type Holder<A> = Boxed(A);\n'
            'export fn same<F<_>, A>(value:F<A>)->F<A> {move(value)}\n')
        source = ('import { Holder as Container, Boxed, same } from "./library";\n'
                  'fn main() { let boxed = same<Container, i16>(Boxed(42)); }\n')
        path = root / "main.rfl"
        path.write_text(source)
        doc = {"uri": path.as_uri()}
        messages = []

        def send(method, params, request_id=None):
            message = {"jsonrpc": "2.0", "method": method, "params": params}
            if request_id is not None:
                message["id"] = request_id
            data = json.dumps(message).encode()
            messages.append(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)

        send("initialize", {"rootUri": root.as_uri(), "capabilities": {}}, 1)
        send("initialized", {})
        send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
        send("textDocument/hover", {"textDocument": doc, "position": position(source, source.index("boxed"))}, 2)
        send("textDocument/definition", {"textDocument": doc, "position": position(source, source.index("same<") + len("same<"))}, 3)
        send("textDocument/formatting", {"textDocument": doc, "options": {"tabSize": 4, "insertSpaces": True}}, 4)
        invalid = 'fn bad<F<_>>(value: F) {}\n'
        send("textDocument/didChange", {"textDocument": {**doc, "version": 2}, "contentChanges": [{"text": invalid}]})
        send("shutdown", None, 5)
        send("exit", None)
        result = subprocess.run([str(binary), "lsp"], input=b"".join(messages), capture_output=True, check=True, timeout=60)
        stream, responses, diagnostics = io.BytesIO(result.stdout), {}, []
        while header := stream.readline():
            length = int(header.split(b":", 1)[1])
            assert stream.readline() == b"\r\n"
            message = json.loads(stream.read(length))
            assert "error" not in message, message
            if "id" in message:
                responses[message["id"]] = message["result"]
            elif message.get("method") == "textDocument/publishDiagnostics":
                diagnostics.append(message["params"]["diagnostics"])
        assert not diagnostics[0], diagnostics
        assert "Container<i16>" in responses[2]["contents"]["value"], responses[2]
        assert "$" not in responses[2]["contents"]["value"], responses[2]
        assert responses[3]["uri"] == (root / "library.rfl").as_uri(), responses[3]
        assert responses[4] is not None
        assert any("constructor" in error["message"] for error in diagnostics[-1]), diagnostics[-1]
    print("Constructor LSP checks passed: aliased constructors, concrete hover, navigation, formatting and kind diagnostics.")


if __name__ == "__main__":
    main()
