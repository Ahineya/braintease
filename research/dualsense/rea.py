#!/usr/bin/env python3
"""Run the local research REA CLI with scoped toolchain settings.

No agent registration, system defaults or controller access. The requested REA
operation determines its behavior; use static inspection commands for this audit.
"""

import os
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parent / "analysis/toolchain"
    node = Path("/Users/ahineya/.nvm/versions/node/v24.11.0/bin/node")
    java = Path("/opt/homebrew/opt/openjdk@25/libexec/openjdk.jdk/Contents/Home")
    developer = Path("/Applications/Xcode.app/Contents/Developer")
    sdk = developer / "Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk"
    ghidra = root / "ghidra/ghidra_12.1.4_PUBLIC"
    entry = root / "rea/node_modules/rea-agents/scripts/rea.mjs"
    for path in [node, java / "bin/java", sdk, ghidra / "support/analyzeHeadless", entry]:
        if not path.exists():
            raise SystemExit(f"Research toolchain component missing: {path}")
    cache = root / "swift-module-cache"
    cache.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update({
        "JAVA_HOME": str(java),
        "GHIDRA_INSTALL_DIR": str(ghidra),
        "GHIDRA_HEADLESS_MAXMEM": "1G",
        "DEVELOPER_DIR": str(developer),
        "SDKROOT": str(sdk),
        "REA_DARWIN_MODULE_CACHE_PATH": str(cache),
        "CLANG_MODULE_CACHE_PATH": str(cache),
        "PATH": os.pathsep.join([str(node.parent), str(java / "bin"), env.get("PATH", "")]),
    })
    return subprocess.run([str(node), str(entry), *sys.argv[1:]], env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
