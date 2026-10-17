#!/usr/bin/env python3
"""Traits through the real editor's stdio LSP protocol, including workspace edits."""
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
    with tempfile.TemporaryDirectory(prefix="rfl traits 🦀 ") as directory:
        root = Path(directory).resolve()
        library = root / "reader #é.rfl"
        main_file = root / "main.rfl"
        lib = ('// 🦀 contract\r\nexport trait Read { fn read(&self, n: i16) -> i16; }\r\n'
               'export struct Value { n: i16 }\r\n'
               'impl Read for Value { fn read(&self, n: i16) -> i16 { self.n + n } }\r\n'
               'trait Other { fn read(&self, n: i16) -> i16; }\r\n'
               'impl Other for bool { fn read(&self, n: i16) -> i16 { n } }\r\n')
        source = ('import { Read as Reader, Value } from "./reader #é";\r\n'
                  'fn bounded<T: Reader>(v: T) -> i16 { v.read(2) }\r\n'
                  'fn main() { /* 🦀 */ let v = Value { n: 3 }; println(v.read(1)); println(Reader::read(&v, 2)); }\r\n')
        library.write_bytes(lib.encode())
        main_file.write_bytes(source.encode())
        doc, lib_doc = {"uri": main_file.as_uri()}, {"uri": library.as_uri()}
        messages = []

        def send(method, params, id=None):
            message = {"jsonrpc": "2.0", "method": method, "params": params}
            if id is not None:
                message["id"] = id
            body = json.dumps(message).encode()
            messages.append(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)

        def request(id, method, text, needle, delta=0, document=doc, **params):
            send("textDocument/" + method, {"textDocument": document,
                 "position": position(text, text.index(needle) + delta), **params}, id)

        def change(text, version):
            send("textDocument/didChange", {"textDocument": {**doc, "version": version}, "contentChanges": [{"text": text}]})

        send("initialize", {"rootUri": root.as_uri(), "capabilities": {"workspace": {"workspaceEdit": {"documentChanges": True}}}}, 1)
        send("initialized", {})
        for document, text, version in [(lib_doc, lib, 12), (doc, source, 7)]:
            send("textDocument/didOpen", {"textDocument": {**document, "text": text, "languageId": "rfl", "version": version}})
        request(2, "definition", source, "v.read(2)", 2)
        request(3, "definition", source, "v.read(1)", 2)
        request(4, "definition", source, "Reader::read", 8)
        request(5, "hover", source, "v.read(2)", 2)
        request(6, "signatureHelp", source, "&v, 2", 4)
        request(7, "references", source, "v.read(2)", 2, context={"includeDeclaration": True})
        request(8, "rename", source, "v.read(2)", 2, newName="fetch")
        request(9, "prepareRename", lib, "trait", 1, document=lib_doc)
        send("textDocument/documentSymbol", {"textDocument": lib_doc}, 10)
        send("textDocument/semanticTokens/full", {"textDocument": lib_doc}, 11)
        request(12, "rename", lib, "Read", document=lib_doc, newName="Readable")
        request(13, "definition", lib, "impl Read for Value { fn read", len("impl Read for Value { fn "), document=lib_doc)
        request(14, "definition", source, "T: Reader", 3)
        member = source.splitlines()[0] + '\nfn bounded<T: Reader>(v: T) { v.re; }\n'
        change(member, 8)
        request(15, "completion", member, "v.re", 4)
        qualified = source.replace("Reader::read", "Reader::re")
        change(qualified, 9)
        request(16, "completion", qualified, "Reader::re", 10)
        change('trait Broken { fn read(&self); } impl Broken for i16 {}', 10)
        send("shutdown", None, 17)
        send("exit", None)
        result = subprocess.run([str(binary), "lsp"], input=b"".join(messages), capture_output=True, timeout=30, check=True)
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
        contract = {"uri": library.as_uri(), "range": {"start": position(lib, lib.index("read")), "end": position(lib, lib.index("read") + 4)}}
        for id in (2, 3, 4, 13):
            assert responses[id] == contract, (id, responses[id])
        assert responses[14]["uri"] == library.as_uri()
        assert responses[14]["range"]["start"] == position(lib, lib.index("Read"))
        assert "fn(i16) -> i16" in responses[5]["contents"]["value"]
        assert responses[6]["signatures"][0]["label"] == "fn(&Value, i16) -> i16", responses[6]
        assert responses[6]["activeParameter"] == 1
        assert len(responses[7]) == 4, responses[7]
        assert responses[9] is None
        assert any(item["name"] == "Read" and item["kind"] == 11 for item in responses[10])
        # Semantic tokens: trait keyword, trait type name and contract method.
        data, line, column, tokens = responses[11]["data"], 0, 0, {}
        for i in range(0, len(data), 5):
            dl, dc, length, kind, _ = data[i:i+5]
            line += dl
            column = column + dc if dl == 0 else dc
            tokens[(line, column)] = kind
        for word, kind in [("trait", 0), ("Read", 2), ("read", 1)]:
            pos = position(lib, lib.index(word))
            assert tokens[(pos["line"], pos["character"])] == kind, (word, tokens)
        for id, old, new, count in [(8, "read", "fetch", 5), (12, "Read", "Readable", 3)]:
            changes = responses[id]["documentChanges"]
            assert len(changes) == 2
            assert sum(len(c["edits"]) for c in changes) == count, changes
            for change in changes:
                expected_version = 7 if change["textDocument"]["uri"] == main_file.as_uri() else 12
                assert change["textDocument"]["version"] == expected_version
                assert all(edit["newText"] == new for edit in change["edits"])
            # The unrelated Other::read contract and implementation stay intact.
            if id == 8:
                edits = next(c["edits"] for c in changes if c["textDocument"]["uri"] == library.as_uri())
                assert all(e["range"]["start"]["line"] < 4 for e in edits)
        item, = [item for item in responses[15] if item["label"] == "read"]
        assert item["kind"] == 2 and item["detail"] == "fn(i16) -> i16"
        assert "additionalTextEdits" not in item
        assert not {"print", "println"} & {item["label"] for item in responses[15]}
        item, = responses[16]
        assert item["label"] == "read" and item["detail"] == "fn(&Self, i16) -> i16"
        assert not diagnostics[0] and not diagnostics[1], diagnostics[:2]
        assert any("missing method" in d["message"] for d in diagnostics[-1]), diagnostics[-1]
        assert library.read_bytes().decode() == lib, "LSP returns edits without mutating files"
    print("Trait LSP checks passed: aliases, bounds, qualified calls, completion, navigation, signatures, semantic tokens and versioned workspace rename.")


if __name__ == "__main__":
    main()
