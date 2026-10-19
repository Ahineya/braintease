#!/usr/bin/env python3
"""Formatting must answer while a real macro blocks semantic analysis."""
import json
import os
import signal
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="rfl-format-latency-") as directory:
        root = Path(directory)
        (root / "spin.rfl").write_text('import {TokenStream} from "std/macro";#[proc_macro]export fn spin(input:TokenStream)->TokenStream{while(true){}input}')
        source = 'import macro {spin} from "./spin";import macro {html} from "std/html_macro";fn main(){html!{<div><p>Hello, 🙂</p></div>};spin!{};}'
        uri = (root / "main.rfl").as_uri()
        process = subprocess.Popen([binary, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, start_new_session=os.name != "nt")
        messages = queue.Queue()

        def reader():
            while line := process.stdout.readline():
                size = int(line.split(b":", 1)[1])
                assert process.stdout.readline() == b"\r\n"
                messages.put(json.loads(process.stdout.read(size)))
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()

        def send(method, params, request_id=None):
            value = {"jsonrpc": "2.0", "method": method, "params": params}
            if request_id is not None:
                value["id"] = request_id
            data = json.dumps(value).encode()
            process.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
            process.stdin.flush()

        try:
            send("initialize", {"capabilities": {}}, 1)
            assert messages.get(timeout=5)["id"] == 1
            send("textDocument/didOpen", {"textDocument": {"uri": uri, "version": 1, "text": source}})
            send("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 0}}, 2)
            start = time.perf_counter()
            send("textDocument/formatting", {"textDocument": {"uri": uri}}, 3)
            result = messages.get(timeout=2)
            elapsed = time.perf_counter() - start
            assert result.get("id") == 3, result
            assert any("\n" in edit["newText"] for edit in result["result"]), result
            # The semantic worker still finishes the actual macro failure and
            # publishes a versioned diagnostic; formatting doesn't cancel it.
            found_diagnostic = False
            while True:
                message = messages.get(timeout=120)
                if message.get("method") == "textDocument/publishDiagnostics":
                    params = message["params"]
                    assert params["version"] == 1
                    assert any("spin" in item["message"] for item in params["diagnostics"]), params
                    found_diagnostic = True
                if message.get("id") == 2:
                    break
            assert found_diagnostic
            send("shutdown", {}, 4)
            send("exit", {})
            process.wait(timeout=10)
            assert process.returncode == 0
            print(f"Formatting while semantic analysis is blocked: {elapsed:.4f}s; diagnostics retained.")
        finally:
            if process.poll() is None:
                if os.name == "nt":
                    process.kill()
                else:
                    os.killpg(process.pid, signal.SIGTERM)
                process.wait()
            process.stdin.close()
            thread.join(timeout=2)
            process.stdout.close()


if __name__ == "__main__":
    main()
