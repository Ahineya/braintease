#!/usr/bin/env python3
"""Generic methods and constructor operations over IDEA's actual LSP transport."""
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
    with tempfile.TemporaryDirectory(prefix="rfl-generic-traits-") as directory:
        root = Path(directory).resolve()
        source = '''import { Functor as Mappable, map } from "std/fp";
type Holder<A> = Boxed(A);
impl Mappable for Holder {
    fn map<A,B>(value:Self<A>, f:fn(A)->B)->Self<B> {
        match move(value) {Boxed(a)=>Boxed(f(move(a)))}
    }
}
fn generic<F<_>:Mappable,A,B>(value:F<A>,f:fn(A)->B)->F<B> {F::map(move(value),f)}
fn main(){let value=generic(Boxed(41),fn(x:i16)=>x+1);}
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
        request(2, "hover", source, source.index("let value") + 4)
        request(3, "definition", source, source.index("F::map") + 3)
        request(4, "hover", source, source.index("F::map") + 3)
        request(5, "signatureHelp", source, source.index("generic(Boxed") + len("generic("))
        send("textDocument/formatting", {"textDocument": doc, "options": {"tabSize": 4, "insertSpaces": True}}, 6)
        partial = source.replace("F::map(", "F::ma(")
        change(partial, 2)
        request(7, "completion", partial, partial.index("F::ma") + 5)
        partial = source.replace("F::map(", "Mappable::ma(")
        change(partial, 3)
        request(8, "completion", partial, partial.index("Mappable::ma") + len("Mappable::ma"))
        change('trait C<_>{} fn bad<T:C>(x:T){}', 4)
        send("shutdown", None, 9)
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
        assert "Holder<i16>" in responses[2]["contents"]["value"], responses[2]
        assert responses[3]["uri"].endswith("/std/fp.rfl"), responses[3]
        assert "Transforms each value" in responses[4]["contents"]["value"], responses[4]
        assert "Holder<i16>" in responses[5]["signatures"][0]["label"], responses[5]
        assert responses[6] is not None
        for request_id in (7, 8):
            item, = [item for item in responses[request_id] if item["label"] == "map"]
            assert item["kind"] == 2, item
            assert "Self<A>" in item["detail"] and not item["detail"].startswith("fn(Self,"), item
        for request_id in (2, 4, 5, 7, 8):
            assert "$contract$" not in json.dumps(responses[request_id]) and "$param$" not in json.dumps(responses[request_id]), responses[request_id]
        assert any("kind arity" in error["message"] for error in diagnostics[-1]), diagnostics[-1]
    print("Generic trait LSP checks passed: bounds, aliases, static completion, concrete signatures, navigation, documentation and kind diagnostics.")


if __name__ == "__main__":
    main()
