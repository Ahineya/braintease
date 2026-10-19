#!/usr/bin/env python3
"""Comprehension bindings remain source-level LSP symbols after expansion."""
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
    source = '''import {Functor,FlatMap} from "std/fp";
fn identity(n:i16)->i16{n}
fn main(){
 let seed=20;
 let answer=for{
 value<-Some(seed);
 let doubled=value*2;
 other<-Some(identity(2));
 yield doubled+other
 };
 println(answer);
 let result=for<Result<_,String>>{n<-Ok(42);yield n};
}
fn chain<F<_>:Functor+FlatMap>(first:F<i16>,next:fn(i16)->F<i16>)->F<i16>{for<F>{n<-move(first);m<-next(n);yield m}}
'''
    with tempfile.TemporaryDirectory(prefix="rfl-comprehension-lsp-") as temporary:
        path = Path(temporary) / "main.rfl"
        path.write_text(source)
        doc = {"uri": path.as_uri()}
        messages = []

        def send(method, params, request=None):
            message = {"jsonrpc": "2.0", "method": method, "params": params}
            if request is not None:
                message["id"] = request
            data = json.dumps(message).encode()
            messages.append(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)

        def at(request, method, offset, **extras):
            send("textDocument/" + method, {"textDocument": doc, "position": position(source, offset), **extras}, request)

        send("initialize", {"rootUri": path.parent.as_uri(), "capabilities": {}}, 1)
        send("initialized", {})
        send("textDocument/didOpen", {"textDocument": {**doc, "languageId": "rfl", "version": 1, "text": source}})
        at(2, "hover", source.index("let answer") + 4)
        at(3, "hover", source.index("value<-"))
        at(4, "definition", source.index("=value") + 1)
        at(5, "completion", source.index("Some(seed)"))
        at(6, "completion", source.index("yield doubled"))
        at(7, "rename", source.index("value<-"), newName="input")
        at(8, "prepareRename", source.index("yield doubled"))
        send("textDocument/formatting", {"textDocument": doc, "options": {"tabSize": 4, "insertSpaces": True}}, 9)
        at(13, "rename", source.index("for<F>")+4, newName="G")
        at(12, "hover", source.index("let result")+4)
        at(11, "signatureHelp", source.index("identity(2)")+len("identity("))
        invalid = 'fn main(){let p=for{n<-Some(1);yield missing+n};}'
        send("textDocument/didChange", {"textDocument": {**doc, "version": 2}, "contentChanges": [{"text": invalid}]})
        send("shutdown", None, 10)
        send("exit", None)
        result = subprocess.run([str(Path(sys.argv[1]).resolve()), "lsp"], input=b"".join(messages), capture_output=True, check=True, timeout=120)
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
        assert "Option<i16>" in responses[2]["contents"]["value"], responses[2]
        assert "i16" in responses[3]["contents"]["value"], responses[3]
        assert responses[4]["range"]["start"] == position(source, source.index("value<-")), responses[4]
        for key in (5, 6):
            items = responses[key]
            if isinstance(items, dict):
                items = items["items"]
            assert all("$" not in item["label"] for item in items), items
            names = {item["label"] for item in items}
            assert ("value" in names) == (key == 6), names
        edits = responses[7]["changes"][path.as_uri()]
        assert len(edits) == 2 and all(edit["newText"] == "input" for edit in edits), edits
        generic_edits = responses[13]["changes"][path.as_uri()]
        ranges = [json.dumps(edit["range"], sort_keys=True) for edit in generic_edits]
        assert len(set(ranges)) == len(ranges), generic_edits
        assert len(generic_edits) == 5, generic_edits
        assert responses[8] is None, responses[8]
        assert "Result<i16, String>" in responses[12]["contents"]["value"], responses[12]
        assert responses[9], responses[9]
        assert "i16" in responses[11]["signatures"][0]["label"], responses[11]
        error = diagnostics[-1][0]
        assert "missing" in error["message"], error
        assert error["range"]["start"] == position(invalid, invalid.index("missing")), error
    print("Comprehension LSP checks passed: hover, scopes, navigation, rename, formatting and source diagnostics.")


if __name__ == "__main__":
    main()
