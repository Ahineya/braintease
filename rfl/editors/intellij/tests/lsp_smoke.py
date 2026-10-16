#!/usr/bin/env python3
"""Check the existing server using the same stdio transport as the IDEA client."""

import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


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
    send("initialize", {"processId": None, "rootUri": Path.cwd().as_uri(), "capabilities": {
        "workspace": {"workspaceEdit": {"documentChanges": True}},
    }}, 1)
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
    missing = "fn value() -> f64 { sin(0.0) }\n"
    at = missing.index("sin")
    import_range = {"start": {"line": 0, "character": at}, "end": {"line": 0, "character": at}}
    send("textDocument/didChange", {"textDocument": {**doc, "version": 3}, "contentChanges": [{"text": missing}]})
    send("textDocument/codeAction", {"textDocument": doc, "range": import_range, "context": {"diagnostics": []}}, 12)
    send("textDocument/codeAction", {"textDocument": doc, "range": import_range,
                                   "context": {"diagnostics": [], "only": ["source.organizeImports"]}}, 13)
    fixed = 'import { sin } from "std/math";\n\n' + missing
    send("textDocument/didChange", {"textDocument": {**doc, "version": 4}, "contentChanges": [{"text": fixed}]})
    send("textDocument/codeAction", {"textDocument": doc, "range": {
        "start": {"line": 2, "character": at}, "end": {"line": 2, "character": at}},
        "context": {"diagnostics": []}}, 14)
    method_source = 'import { replace } from "std/string";\nfn main() { let a = "something"; let b = a.replace("some", "oth"); println(b); b.tr(); }\n'
    send("textDocument/didChange", {"textDocument": {**doc, "version": 5}, "contentChanges": [{"text": method_source}]})
    send("textDocument/completion", {"textDocument": doc, "position": {
        "line": 1, "character": method_source.splitlines()[1].index("b.tr") + 4}}, 15)
    method_fixed = method_source.replace("{ replace }", "{ trim, replace }").replace("b.tr()", "b.trim()")
    send("textDocument/didChange", {"textDocument": {**doc, "version": 6}, "contentChanges": [{"text": method_fixed}]})
    send("textDocument/didChange", {"textDocument": {**doc, "version": 7}, "contentChanges": [{"text": source}]})
    editing = '''import { replace } from "std/string";
import { sin, PI } from "std/math";
fn main() {
 let sample_rate: u32 = 44100;
 let frequency = 440.0;
 for (i in 0..sample_rate) {
  let t = (i as f64) / (sample_rate as f64);
  let sample = sin(2.0 * PI * frequency * t);
  println(sample * 0.5 + 0.5);
 }
 let a = "something";
 let b = a.replace("some", "oth");
 println(b);
 freq
}
'''
    send("textDocument/didChange", {"textDocument": {**doc, "version": 8}, "contentChanges": [{"text": editing}]})
    send("textDocument/completion", {"textDocument": doc, "position": {"line": 13, "character": 5}}, 16)
    dangling = editing.replace(" freq\n", " b.\n")
    send("textDocument/didChange", {"textDocument": {**doc, "version": 9}, "contentChanges": [{"text": dangling}]})
    send("textDocument/completion", {"textDocument": doc, "position": {"line": 13, "character": 3}}, 17)
    valid = dangling.replace("{ replace }", "{ trim, replace }").replace(" b.\n", " b.trim();\n")
    send("textDocument/didChange", {"textDocument": {**doc, "version": 10}, "contentChanges": [{"text": valid}]})
    # Import names and quoted module paths navigate independently of expression uses.
    for id, line, text in [(18, 1, "sin"), (19, 1, "std/math"), (20, 1, "PI"), (21, 0, "replace")]:
        send("textDocument/definition", {"textDocument": doc, "position": {
            "line": line, "character": valid.splitlines()[line].index(text) + 1}}, id)
    aliased = valid.replace("{ sin, PI }", "{ sin as sine, PI }").replace("= sin(", "= sine(")
    send("textDocument/didChange", {"textDocument": {**doc, "version": 11}, "contentChanges": [{"text": aliased}]})
    send("textDocument/definition", {"textDocument": doc, "position": {
        "line": 1, "character": aliased.splitlines()[1].index("sine") + 1}}, 22)
    # A relative module with spaces and URI-reserved/Unicode characters exercises
    # both import resolution and the file URI returned to IDEA.
    build_dir = Path(__file__).resolve().parent.parent / "build"
    build_dir.mkdir(exist_ok=True)
    navigation_dir = tempfile.TemporaryDirectory(prefix="import navigation 🦀 ", dir=build_dir)
    target = Path(navigation_dir.name) / "helpers #é.rfl"
    target_text = "// 🦀 module\r\nexport fn wave() -> i16 { 1 }\r\nexport const BASE: i16 = 2;\r\nexport struct Tone { value: i16 }\r\nexport type Signal = Pulse;\r\n"
    target.write_bytes(target_text.encode())
    relative_doc = {"uri": (target.parent / "main.rfl").as_uri()}
    relative_source = 'import { wave as play, BASE, Tone, Signal, Pulse } from "./helpers #é.rfl";\nfn main() {}\n'
    send("textDocument/didOpen", {"textDocument": {**relative_doc, "languageId": "rfl", "version": 1, "text": relative_source}})
    for id, text in [(23, "play"), (24, "./helpers"), (25, "BASE"), (26, "Tone"), (27, "Signal"), (28, "Pulse")]:
        send("textDocument/definition", {"textDocument": relative_doc, "position": {
            "line": 0, "character": relative_source.index(text) + 1}}, id)
    send("textDocument/didClose", {"textDocument": relative_doc})
    builtin_source = "fn main() { pri(1); }\n"
    send("textDocument/didChange", {"textDocument": {**doc, "version": 12}, "contentChanges": [{"text": builtin_source}]})
    send("textDocument/completion", {"textDocument": doc, "position": {
        "line": 0, "character": builtin_source.index("pri(") + 3}}, 29)
    builtin_fixed = builtin_source.replace("pri(", "println(")
    send("textDocument/didChange", {"textDocument": {**doc, "version": 13}, "contentChanges": [{"text": builtin_fixed}]})
    send("textDocument/signatureHelp", {"textDocument": doc, "position": {
        "line": 0, "character": builtin_fixed.index("println(") + 8}}, 30)
    send("textDocument/didClose", {"textDocument": doc})
    send("shutdown", None, 11)
    send("exit", None)
    result = subprocess.run([str(binary), "lsp"], input=b"".join(messages), capture_output=True, timeout=30, check=True)
    navigation_dir.cleanup()
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
    assert responses[1]["capabilities"]["codeActionProvider"]["codeActionKinds"] == ["quickfix"]
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
    action, = responses[12]
    assert action["title"] == "Import `sin` from `std/math`" and action["kind"] == "quickfix"
    change, = action["edit"]["documentChanges"]
    assert change["textDocument"] == {"uri": uri, "version": 3}
    edit, = change["edits"]
    assert edit["range"] == {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}}
    assert edit["newText"] + missing == fixed
    assert responses[13] == [] and responses[14] == []
    trim = next(item for item in responses[15] if item["label"] == "trim")
    assert trim["kind"] == 2 and trim["textEdit"]["newText"] == "trim"
    assert trim["additionalTextEdits"][0]["newText"] == "trim, "
    replace = next(item for item in responses[15] if item["label"] == "replace")
    assert "additionalTextEdits" not in replace
    assert not any(item["label"] in ("region_eq", "push") for item in responses[15])
    names = {item["label"] for item in responses[16]}
    assert names >= {"sin", "replace", "PI", "a", "b", "frequency", "sample_rate"}, names
    assert not names & {"fract", "from", "region_eq", "__rcc_f64_reduce_pi2", "i", "t", "sample"}, names
    assert not any("$" in name or name.startswith("__rcc_") for name in names), names
    frequency = next(item for item in responses[16] if item["label"] == "frequency")
    assert frequency["textEdit"] == {"range": {"start": {"line": 13, "character": 1}, "end": {"line": 13, "character": 5}}, "newText": "frequency"}, frequency
    assert any(item["label"] == "trim" and item.get("additionalTextEdits") for item in responses[17])
    assert len(diagnostics) == 14 and [bool(items) for items in diagnostics] == [False, True, True, False, True, False, False, True, True, False, False, False, True, False], diagnostics
    error, = diagnostics[8]
    assert error["message"] == "expected a member name after '.'", error
    assert error["range"] == {"start": {"line": 13, "character": 2}, "end": {"line": 13, "character": 3}}, error
    std = Path(__file__).resolve().parents[3] / "std"

    def location(path, text, name):
        declaration = re.search(r"\b(?:fn|const|struct|type)\s+(" + re.escape(name) + r")\b", text)
        at = declaration.start(1) if declaration else text.index(name)
        before = text[:at]
        line = before.count("\n")
        character = len(before.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2
        return {"uri": path.as_uri(), "range": {"start": {"line": line, "character": character},
                "end": {"line": line, "character": character + len(name)}}}

    math = std / "math.rfl"
    string = std / "string.rfl"
    assert responses[18] == responses[22] == location(math, math.read_text(), "sin"), responses[18]
    assert responses[19] == {"uri": math.as_uri(), "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}}}, responses[19]
    assert responses[20] == location(math, math.read_text(), "PI"), responses[20]
    assert responses[21] == location(string, string.read_text(), "replace"), responses[21]
    for id, name in [(23, "wave"), (25, "BASE"), (26, "Tone"), (27, "Signal"), (28, "Pulse")]:
        assert responses[id] == location(target, target_text, name), responses[id]
    assert responses[24] == {"uri": target.as_uri(), "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}}}, responses[24]
    builtins = {"print", "println", "putc", "from_char", "from_byte"}
    for id in (16, 29):
        for name in builtins:
            item, = [item for item in responses[id] if item["label"] == name]
            assert item["kind"] == 3 and "additionalTextEdits" not in item, item
    assert not builtins & {item["label"] for item in responses[17]}
    println = next(item for item in responses[29] if item["label"] == "println")
    assert println["textEdit"] == {"range": {"start": {"line": 0, "character": 12}, "end": {"line": 0, "character": 15}}, "newText": "println"}, println
    assert responses[30]["signatures"][0]["label"] == println["detail"], responses[30]
    print("LSP smoke passed: diagnostics and recovery while typing, scoped and builtin completion, navigation (including imports and paths), rename, symbols, hints, stdlib quick fixes, method completion with imports, and shutdown.")


if __name__ == "__main__":
    main()
