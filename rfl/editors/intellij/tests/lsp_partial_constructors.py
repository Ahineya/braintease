#!/usr/bin/env python3
"""Partial constructors over IDEA's shared stdio LSP transport."""
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
    with tempfile.TemporaryDirectory(prefix="rfl-partials-lsp-") as directory:
        root = Path(directory).resolve()
        source = '''import {Functor, map, map_result} from "std/fp";
trait Keep<_> {fn keep<A>(x:Self<A>)->Self<A>;}
impl<E> Keep for Result<_,E> {fn keep<A>(x:Self<A>)->Self<A>{move(x)}}
fn forwarded<E,A,B>(value:Result<A,E>,f:fn(A)->B)->Result<B,E> {
    map<Result<_,E>,_,_>(move(value),f)
}
fn main(){
    let result=forwarded(Ok<i16,String>(41),fn(x:i16)=>x+1);
    let direct=map<Result<_,String>,_,_>(result,fn(x:i16)=>x+1);
    println(map_result(direct,fn(x:i16)=>x));
}
'''
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

        def request(request_id, method, text, at):
            send("textDocument/" + method, {"textDocument": doc, "position": position(text, at)}, request_id)

        def change(text, version):
            send("textDocument/didChange", {"textDocument": {**doc, "version": version}, "contentChanges": [{"text": text}]})

        send("initialize", {"rootUri": root.as_uri(), "capabilities": {}}, 1)
        send("initialized", {})
        send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
        request(2, "hover", source, source.index("let result") + 4)
        request(3, "hover", source, source.index("let direct") + 4)
        request(4, "definition", source, source.index("map<Result<_,E>"))
        request(5, "signatureHelp", source, source.index("map<Result<_,String>,_,_>(") + len("map<Result<_,String>,_,_>("))
        send("textDocument/formatting", {"textDocument": doc, "options": {"tabSize": 4, "insertSpaces": True}}, 6)
        partial = source.replace("{move(x)}", "{Self::ke(move(x))}")
        change(partial, 2)
        request(7, "completion", partial, partial.index("Self::ke") + len("Self::ke"))
        change("fn same<F<_>,A>(x:F<A>)->F<A>{move(x)} fn main(){same(Ok<i16,String>(42));}", 3)
        change("fn bad(x:Result<_,String>){}", 4)
        send("shutdown", None, 8)
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
        for request_id in (2, 3):
            assert "Result<i16, String>" in responses[request_id]["contents"]["value"], responses[request_id]
        assert responses[4]["uri"].endswith("/std/fp.rfl"), responses[4]
        assert "Result<i16, String>" in responses[5]["signatures"][0]["label"], responses[5]
        def offset(at):
            lines = source.splitlines(keepends=True)
            return sum(len(line) for line in lines[:at["line"]]) + len(lines[at["line"]].encode("utf-16-le")[:at["character"] * 2].decode("utf-16-le"))

        formatted = source
        for edit in sorted(responses[6], key=lambda edit: offset(edit["range"]["start"]), reverse=True):
            start, end = offset(edit["range"]["start"]), offset(edit["range"]["end"])
            formatted = formatted[:start] + edit["newText"] + formatted[end:]
        assert "Result<_, E>" in formatted, formatted
        item, = [item for item in responses[7] if item["label"] == "keep"]
        assert item["kind"] == 2, item
        assert any("explicit slots" in error["message"] for group in diagnostics for error in group), diagnostics
        assert any("placeholder" in error["message"] for error in diagnostics[-1]), diagnostics[-1]
        for request_id in (2, 3, 5, 7):
            assert "$contract$" not in json.dumps(responses[request_id]) and "$param$" not in json.dumps(responses[request_id]), responses[request_id]
    print("Partial constructor LSP checks passed: concrete hover/signatures, navigation, Self completion, formatting and slot diagnostics.")


if __name__ == "__main__":
    main()
