#!/usr/bin/env python3
"""Lazy effect metadata and ownership diagnostics over IDEA's LSP transport."""
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
    with tempfile.TemporaryDirectory(prefix="rfl-effect-lsp-") as directory:
        path = Path(directory).resolve() / "main.rfl"
        source = '''import {Effect,succeed,map,run} from "std/effect";
fn main(){
    let program:Effect<unit,String,i16> = succeed(40);
    let mapped=map(move(program),fn(n:i16)=>n+2);
    let future=run(move(mapped),{});
    drop(move(future));
}
'''
        path.write_text(source)
        doc = {"uri": path.as_uri()}
        messages = []

        def send(method, params, request_id=None):
            message = {"jsonrpc": "2.0", "method": method, "params": params}
            if request_id is not None:
                message["id"] = request_id
            data = json.dumps(message).encode()
            messages.append(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)

        def request(request_id, method, at):
            send("textDocument/" + method, {"textDocument": doc, "position": position(source, at)}, request_id)

        send("initialize", {"rootUri": path.parent.as_uri(), "capabilities": {}}, 1)
        send("initialized", {})
        send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
        request(2, "hover", source.index("let mapped") + 4)
        request(3, "signatureHelp", source.index("map(move(program)") + 4)
        request(4, "definition", source.index("move(mapped)") + 5)
        send("textDocument/formatting", {"textDocument": doc, "options": {"tabSize": 4, "insertSpaces": True}}, 5)
        reusable = 'import {Effect,succeed} from "std/effect"; import {Shared,share} from "std/shared"; fn main(){let s=share(owned(42,fn(n:&mut i16)=>{}));let p:Effect<unit,String,Shared<Owned<i16>>> = succeed(s);let copy=p;}'
        send("textDocument/didChange", {"textDocument": {**doc, "version": 2}, "contentChanges": [{"text": reusable}]})
        send("textDocument/didChange", {"textDocument": {**doc, "version": 3}, "contentChanges": [{"text": 'import {OwnedEffect,succeed} from "std/effect_once"; fn main(){let p:OwnedEffect<unit,String,i16> = succeed(42);let copy=p;}'}]})
        send("textDocument/didChange", {"textDocument": {**doc, "version": 4}, "contentChanges": [{"text": 'import {Effect,succeed} from "std/effect"; fn main(){let value=owned(42,fn(n:&mut i16)=>{});let p:Effect<unit,String,Owned<i16>> = succeed(move(value));}'}]})
        send("shutdown", None, 6)
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
        assert not diagnostics[0], diagnostics[0]
        assert not diagnostics[1], diagnostics[1]
        assert "Effect<unit, String, i16>" in responses[2]["contents"]["value"], responses[2]
        assert "fn(i16)" in responses[3]["signatures"][0]["label"], responses[3]
        assert responses[4]["range"]["start"] == position(source, source.index("let mapped") + 4), responses[4]
        def offset(at):
            lines = source.splitlines(keepends=True)
            return sum(len(line) for line in lines[:at["line"]]) + len(lines[at["line"]].encode("utf-16-le")[:at["character"] * 2].decode("utf-16-le"))

        formatted = source
        for edit in sorted(responses[5], key=lambda edit: offset(edit["range"]["start"]), reverse=True):
            start, end = offset(edit["range"]["start"]), offset(edit["range"]["end"])
            formatted = formatted[:start] + edit["newText"] + formatted[end:]
        assert "Effect<unit, String, i16>" in formatted and "fn(n: i16) =>" in formatted, formatted
        assert any("cannot be copied" in item["message"] for item in diagnostics[-2]), diagnostics[-2]
        assert any("does not implement `Copy`" in item["message"] for item in diagnostics[-1]), diagnostics[-1]
    print("Lazy effect LSP checks passed: hover, signatures, navigation, formatting and ownership diagnostics.")


if __name__ == "__main__":
    main()
