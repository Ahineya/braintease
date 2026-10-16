#!/usr/bin/env python3
"""Associated types through the same stdio server used by IntelliJ."""
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def position(text, offset):
    prefix = text[:offset]
    return {"line": prefix.count("\n"), "character": len(prefix.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}


def main():
    with tempfile.TemporaryDirectory(prefix="rfl associated 🦀 ") as directory:
        root = Path(directory).resolve()
        library, main_file = root / "source #é.rfl", root / "main.rfl"
        lib = ('// 🦀 contract\r\nexport trait Source { type Output: Printable; fn output(&self) -> Self::Output; }\r\n'
               'export struct Value { n:i16 }\r\n'
               'impl Source for Value { type Output=String; fn output(&self) -> Self::Output { "hello" } }\r\n')
        source = ('import { Source as Produce, Value } from "./source #é";\r\n'
                  'fn get<T:Produce>(x:T) -> T::Output { x.output() }\r\n'
                  'fn main() { let x=Value{n:42}; let out:<Value as Produce>::Output=get(x); println(out); }\r\n')
        library.write_bytes(lib.encode())
        main_file.write_bytes(source.encode())
        doc, lib_doc = {"uri": main_file.as_uri()}, {"uri": library.as_uri()}
        messages = []

        def send(method, params, id=None):
            body = {"jsonrpc": "2.0", "method": method, "params": params}
            if id is not None:
                body["id"] = id
            encoded = json.dumps(body).encode()
            messages.append(f"Content-Length: {len(encoded)}\r\n\r\n".encode() + encoded)

        def request(id, method, text, needle, delta=0, document=doc, **params):
            send("textDocument/" + method, {"textDocument": document,
                 "position": position(text, text.index(needle) + delta), **params}, id)

        def change(text, version):
            send("textDocument/didChange", {"textDocument": {**doc, "version": version}, "contentChanges": [{"text": text}]})

        send("initialize", {"rootUri": root.as_uri(), "capabilities": {"workspace": {"workspaceEdit": {"documentChanges": True}}}}, 1)
        send("initialized", {})
        for document, text, version in [(lib_doc, lib, 4), (doc, source, 9)]:
            send("textDocument/didOpen", {"textDocument": {**document, "text": text, "version": version, "languageId": "rfl"}})
        request(2, "definition", source, "T::Output", 3)
        request(3, "definition", source, ">::Output", 3)
        request(4, "definition", lib, "type Output=", 5, document=lib_doc)
        request(5, "hover", source, ">::Output", 3)
        request(6, "rename", source, "T::Output", 3, newName="Payload")
        send("textDocument/documentSymbol", {"textDocument": lib_doc}, 7)
        for id, original, replacement in [(8, "T::Output", "T::Ou"), (9, "T::Output", "T::"), (10, "<Value as Produce>::Output", "<Value as Produce>::Ou")]:
            edited = source.replace(original, replacement)
            change(edited, id + 2)
            request(id, "completion", edited, replacement, len(replacement))
        edited = source.replace("println(out);", "x.output();")
        change(edited, 13)
        request(11, "signatureHelp", edited, "x.output(", len("x.output("))
        change('trait Broken { type Output; } impl Broken for i16 {}', 14)
        send("shutdown", None, 12)
        send("exit", None)
        result = subprocess.run([str(Path(sys.argv[1]).resolve()), "lsp"], input=b"".join(messages), capture_output=True, timeout=30, check=True)
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
        start = lib.index("Output")
        target = {"uri": library.as_uri(), "range": {"start": position(lib, start), "end": position(lib, start + 6)}}
        for id in (2, 3, 4):
            assert responses[id] == target, (id, responses[id])
        assert "String" in responses[5]["contents"]["value"]
        changes = responses[6]["documentChanges"]
        assert len(changes) == 2 and sum(len(c["edits"]) for c in changes) == 6, changes
        for change in changes:
            assert change["textDocument"]["version"] == (9 if change["textDocument"]["uri"] == main_file.as_uri() else 4)
            assert all(e["newText"] == "Payload" for e in change["edits"])
        assert any(item["name"] == "Output" and item["kind"] == 26 for item in responses[7]), responses[7]
        for id in (8, 9, 10):
            item, = [i for i in responses[id] if i["label"] == "Output"]
            assert item["kind"] == 25 and "Printable" in item["detail"], (id, item)
        assert responses[11]["signatures"][0]["label"].endswith("-> String"), responses[11]
        assert not diagnostics[0] and not diagnostics[1], diagnostics[:2]
        assert any("missing associated type" in d["message"] for d in diagnostics[-1]), diagnostics[-1]
        assert library.read_bytes().decode() == lib
    print("Associated type LSP checks passed: projections, aliases, navigation, completion, signatures and versioned workspace rename.")


if __name__ == "__main__":
    main()
