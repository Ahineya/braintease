#!/usr/bin/env python3
"""Build the RFL (or C) browser example and serve it locally."""
import argparse
import functools
import http.server
import pathlib
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    examples = parser.add_mutually_exclusive_group()
    examples.add_argument("--c", action="store_true", help="use the stateful C counter")
    examples.add_argument("--async", dest="async_app", action="store_true", help="use the mounted RFL async/await application")
    examples.add_argument("--ui", action="store_true", help="use the typed HTML macro and Model/Msg/commands application")
    examples.add_argument("--todo", action="store_true", help="use the RFL TodoMVC SPA with hash routing and local storage")
    examples.add_argument("--effects", action="store_true", help="use the C timer/fetch/cancellation demo")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[3]
    directory = "todomvc" if args.todo else "browser"
    output = root / f"rcc-wasm/examples/{directory}/build/app.wasm"
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cargo", "build", "--release", "--jobs", "2", "-p", "rcc-driver" if args.c or args.effects else "rfl"], cwd=root, check=True)
    if args.c or args.effects:
        source = "browser-effects.c" if args.effects else "browser-counter.c"
        command = ["target/release/rcc", "compile", f"rcc-wasm/examples/{source}", "-I", "rcc-wasm"]
    else:
        source = "browser-ui.rfl" if args.ui else "browser-async.rfl" if args.async_app else "browser.rfl"
        input_path = "rfl/apps/todomvc/main.rfl" if args.todo else f"rfl/examples/{source}"
        command = ["target/release/rfl", "compile", input_path]
    subprocess.run([*command, "--target", "wasm", "-o", str(output)], cwd=root, check=True)
    print(f"Built {output.relative_to(root)} ({output.stat().st_size:,} bytes)", flush=True)
    if args.build_only:
        return
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(root))
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"Open http://127.0.0.1:{args.port}/rcc-wasm/examples/{directory}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
