#!/usr/bin/env python3
"""Applicative, traversal and injectable Console metadata through the real LSP."""
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
    source = '''import {map2} from "std/applicative";
import {traverse} from "std/traverse";
import {Effect,succeed} from "std/effect";
import {RecordingConsole} from "std/console_test";
import {println as line} from "std/console";
fn main(){
 let left:Effect<unit,String,i16> = succeed(20);
 let right:Effect<unit,String,i16> = succeed(22);
 let paired=map2<Effect<unit,String,_>,i16,i16,i16>(left,right,fn(a:i16,b:i16)=>a+b);
 let values=traverse<Effect<unit,String,_>,i16,i16>([1,2],fn(n:i16)=>succeed<unit,String,i16>(n));
 let greeting=line<RecordingConsole>("Hello!");
}
'''
    with tempfile.TemporaryDirectory(prefix="rfl-applicative-console-lsp-") as folder:
        path = Path(folder) / "main.rfl"
        path.write_text(source)
        doc = {"uri": path.as_uri()}
        messages = []

        def send(method, params, request_id=None):
            message = {"jsonrpc": "2.0", "method": method, "params": params}
            if request_id is not None:
                message["id"] = request_id
            data = json.dumps(message).encode()
            messages.append(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)

        def at(request_id, method, offset):
            send("textDocument/" + method, {"textDocument": doc, "position": position(source, offset)}, request_id)

        send("initialize", {"rootUri": path.parent.as_uri(), "capabilities": {}}, 1)
        send("initialized", {})
        send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
        at(2, "hover", source.index("let paired") + 4)
        at(3, "hover", source.index("let values") + 4)
        at(4, "hover", source.index("let greeting") + 4)
        at(5, "definition", source.index("=map2") + 1)
        at(6, "hover", source.index("=line") + 1)
        at(7, "signatureHelp", source.index("left,right") + 5)
        send("textDocument/formatting", {"textDocument": doc, "options": {"tabSize": 4, "insertSpaces": True}}, 8)
        invalid = 'import {map2} from "std/applicative";fn main(){map2<Vec,i16,i16,i16>([1],[2],fn(a:i16,b:i16)=>a+b);}'
        send("textDocument/didChange", {"textDocument": {**doc, "version": 2}, "contentChanges": [{"text": invalid}]})
        send("shutdown", None, 9)
        send("exit", None)
        result = subprocess.run([str(binary), "lsp"], input=b"".join(messages), capture_output=True, check=True, timeout=120)
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
        for key, expected in [(2, "Effect<unit, String, i16>"), (3, "Effect<unit, String, Vec<i16>>"), (4, "Effect<RecordingConsole, ConsoleError, unit>")]:
            assert expected in responses[key]["contents"]["value"], responses[key]
        assert responses[5]["uri"].endswith("/std/applicative.rfl"), responses[5]
        assert "Defers writing text followed by one LF" in responses[6]["contents"]["value"], responses[6]
        assert "fn(i16, i16)" in responses[7]["signatures"][0]["label"], responses[7]
        assert responses[8], responses[8]
        assert any("does not implement `Apply`" in item["message"] for item in diagnostics[-1]), diagnostics[-1]
    print("Applicative/Console LSP checks passed: normalized types, signatures, docs, navigation, formatting and diagnostics.")


if __name__ == "__main__":
    main()
