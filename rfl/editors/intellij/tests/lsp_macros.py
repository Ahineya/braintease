#!/usr/bin/env python3
"""Real macro execution through stdio LSP, including unsaved dependencies."""
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def position(source, offset):
    before = source[:offset]
    return {"line": before.count("\n"), "character": len(before.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}


def main():
    implementation = '''import {TokenStream,splice,quote} from "std/macro";
#[proc_macro] export fn keep(input:TokenStream)->TokenStream{splice("{let temporary=$0;temporary}",[input])}
#[proc_macro] export fn declare(input:TokenStream)->TokenStream{splice("fn $0()->i16{42}",[input])}
'''
    source = '''import macro {keep,declare} from "./macro.rfl";
declare!{answer};
fn main(){
 let seed=20;
 let value:i16=keep!{seed+22};
 println(value+answer());
}
'''
    with tempfile.TemporaryDirectory(prefix="rfl-macro-lsp-") as temporary:
        root = Path(temporary).resolve()
        path, package = root / "main.rfl", root / "macro.rfl"
        path.write_text(source)
        package.write_text(implementation)
        doc = {"uri": path.as_uri()}
        messages = []

        def send(method, params, request=None):
            value = {"jsonrpc": "2.0", "method": method, "params": params}
            if request is not None:
                value["id"] = request
            body = json.dumps(value).encode()
            messages.append(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)

        def at(request, method, offset, **extras):
            send("textDocument/" + method, {"textDocument": doc, "position": position(source, offset), **extras}, request)

        send("initialize", {"rootUri": root.as_uri(), "capabilities": {}}, 1)
        send("initialized", {})
        send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
        at(2, "hover", source.index("seed+22"))
        at(3, "definition", source.index("seed+22"))
        at(4, "definition", source.index("answer()"))
        at(5, "definition", source.index("keep!"))
        at(6, "completion", source.index("seed+22"))
        at(7, "rename", source.index("seed+22"), newName="number")
        at(8, "rename", source.index("answer()"), newName="result")
        send("textDocument/formatting", {"textDocument": doc, "options": {"tabSize": 4, "insertSpaces": True}}, 9)
        broken = implementation.replace('splice("{let temporary=$0;temporary}",[input])', 'quote("false")')
        send("textDocument/didOpen", {"textDocument": {"uri": package.as_uri(), "languageId": "rfl", "version": 1, "text": broken}})
        at(10, "hover", source.index("value+answer"))
        send("textDocument/didChange", {"textDocument": {"uri": package.as_uri(), "version": 2}, "contentChanges": [{"text": implementation}]})
        at(11, "hover", source.index("value+answer"))
        incomplete = 'fn broken(){ keep!{raw text isn\'t complete\nfn other(){missing;}'
        send("textDocument/didChange", {"textDocument": {**doc, "version": 2}, "contentChanges": [{"text": incomplete}]})
        send("shutdown", None, 12)
        send("exit", None)
        output = subprocess.run([str(Path(sys.argv[1]).resolve()), "lsp"], input=b"".join(messages), capture_output=True, check=True, timeout=180)
        responses, diagnostics = {}, []
        stream = io.BytesIO(output.stdout)
        while header := stream.readline():
            length = int(header.split(b":", 1)[1])
            assert stream.readline() == b"\r\n"
            message = json.loads(stream.read(length))
            assert "error" not in message, message
            if "id" in message:
                responses[message["id"]] = message["result"]
            elif message.get("method") == "textDocument/publishDiagnostics" and message["params"]["uri"] == path.as_uri():
                diagnostics.append(message["params"]["diagnostics"])
        assert not diagnostics[0], diagnostics
        assert "i16" in responses[2]["contents"]["value"], responses[2]
        assert responses[3]["range"]["start"] == position(source, source.index("seed=20")), responses[3]
        assert responses[4]["range"]["start"] == position(source, source.index("answer}")), responses[4]
        assert responses[5]["uri"] == package.as_uri(), responses[5]
        items = responses[6]
        assert all("$" not in item["label"] and item["label"] != "temporary" for item in items), items
        assert {"seed", "keep", "declare"}.issubset({item["label"] for item in items}), items
        for key, spelling in [(7, "number"), (8, "result")]:
            edits = responses[key]["changes"][path.as_uri()]
            assert len(edits) == 2 and all(edit["newText"] == spelling for edit in edits), edits
        assert responses[9], responses[9]
        assert any(any("bool" in item["message"] for item in stage) for stage in diagnostics), diagnostics
        assert "i16" in responses[11]["contents"]["value"], responses[11]
        assert not diagnostics[-2], diagnostics
        assert any("unclosed macro" in item["message"] for item in diagnostics[-1]), diagnostics[-1]
        assert any("missing" in item["message"] for item in diagnostics[-1]), diagnostics[-1]
    print("Macro LSP checks passed: checked types, navigation, completion, rename, formatting, recovery and unsaved dependency invalidation.")


if __name__ == "__main__":
    main()
