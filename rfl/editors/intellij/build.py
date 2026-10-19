#!/usr/bin/env python3
"""Build an installable plugin against a local IDEA, without downloading an SDK."""

import argparse
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idea-home", default=os.environ.get("IDEA_HOME"),
                        help="IntelliJ installation (.app or installation directory); also IDEA_HOME")
    parser.add_argument("--test", action="store_true", help="Run editor and executable-discovery checks")
    parser.add_argument("--server", type=Path, help="Also smoke-test this existing rfl executable over LSP")
    args = parser.parse_args()
    home = Path(args.idea_home or "/Applications/IntelliJ IDEA.app").expanduser().resolve()
    if (home / "Contents").is_dir():
        home /= "Contents"
    info_file = next((p for p in (home / "Resources/product-info.json", home / "product-info.json")
                      if p.is_file()), None)
    if info_file is None:
        parser.error("Cannot locate product-info.json; set --idea-home or IDEA_HOME.")
    info = json.loads(info_file.read_text())
    descriptor = ET.parse(ROOT / "src/main/resources/META-INF/plugin.xml").getroot()
    minimum = descriptor.find("idea-version").attrib["since-build"]
    build = info["buildNumber"].removeprefix(info["productCode"] + "-")
    if tuple(map(int, build.split("."))) < tuple(map(int, minimum.split("."))):
        parser.error(f"IDEA {info['version']} is too old; use 2026.1.5 or later.")
    # The plugin is currently constrained to the 2026.1 release family.
    if build.split(".")[0] != "261":
        parser.error("This plugin targets IDEA 2026.1.x; use a 2026.1.5+ SDK.")
    runtimes = [home / "jbr/Contents/Home", home / "jbr", home / "jre"]
    if os.environ.get("JAVA_HOME"):
        runtimes.append(Path(os.environ["JAVA_HOME"]))
    suffix = ".exe" if os.name == "nt" else ""
    runtime = next((p for p in runtimes if (p / f"bin/javac{suffix}").is_file()), None)
    if runtime is None:
        parser.error("A JDK 21+ is required; set JAVA_HOME if the IDE runtime has no javac.")
    javac = runtime / f"bin/javac{suffix}"
    java = runtime / f"bin/java{suffix}"
    jars = sorted((home / "lib").glob("*.jar"))
    classpath = os.pathsep.join(map(str, jars))
    build_dir = ROOT / "build"
    classes = build_dir / "classes"
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir(parents=True)
    sources = sorted((ROOT / "src/main/java").rglob("*.java"))
    print(f"Compiling against {info['name']} {info['version']} ({build})", flush=True)
    subprocess.run([str(javac), "--release", "21", "-encoding", "UTF-8", "-proc:none",
                    "-classpath", classpath, "-d", str(classes), *map(str, sources)], check=True)
    if args.test:
        test_classes = build_dir / "test-classes"
        test_classes.mkdir(exist_ok=True)
        test_sources = sorted((ROOT / "src/test/java").rglob("*.java"))
        test_classpath = os.pathsep.join([str(classes), classpath])
        subprocess.run([str(javac), "--release", "21", "-encoding", "UTF-8", "-proc:none",
                        "-classpath", test_classpath, "-d", str(test_classes), *map(str, test_sources)], check=True)
        subprocess.run([str(java), "-ea", "-Djava.awt.headless=true", "-classpath",
                        os.pathsep.join([str(test_classes), test_classpath]),
                        "org.ripple.rfl.EditorTest", str(build_dir)], cwd=ROOT, check=True)
        subprocess.run([str(java), "-ea", "-Djava.awt.headless=true", "-classpath",
                        os.pathsep.join([str(test_classes), test_classpath]),
                        "org.ripple.rfl.RunConfigurationTest", str(build_dir),
                        *([str(args.server.resolve())] if args.server else [])], cwd=ROOT, check=True)
    if args.server:
        for script in ("lsp_smoke.py", "lsp_traits.py", "lsp_associated.py", "lsp_formatting.py", "lsp_type_hints.py", "lsp_documentation.py", "lsp_type_constructors.py",
                       "lsp_generic_traits.py", "lsp_partial_constructors.py", "lsp_once_functions.py", "lsp_comprehensions.py", "lsp_macros.py", "lsp_formatting_latency.py"):
            subprocess.run([sys.executable, str(ROOT / "tests" / script), str(args.server.resolve())], check=True)
    # Check that every explicitly registered implementation exists in the output.
    for element in descriptor.iter():
        for attribute in ("implementation", "implementationClass", "serviceImplementation", "instance", "class"):
            if attribute in element.attrib:
                entry = element.attrib[attribute].replace(".", "/") + ".class"
                if not (classes / entry).is_file():
                    raise RuntimeError(f"Plugin descriptor refers to missing class: {entry}")
    jar = io.BytesIO()
    with ZipFile(jar, "w", ZIP_DEFLATED) as archive:
        for folder in (classes, ROOT / "src/main/resources"):
            for file in sorted(folder.rglob("*")):
                if file.is_file():
                    archive.write(file, file.relative_to(folder).as_posix())
    version = descriptor.findtext("version")
    output = build_dir / "distributions" / f"rfl-intellij-{version}.zip"
    output.parent.mkdir(exist_ok=True)
    with ZipFile(output, "w", ZIP_DEFLATED) as archive:
        archive.writestr(f"rfl/lib/rfl-intellij-{version}.jar", jar.getvalue())
    print(f"Built {output}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(f"Build/check failed (exit {error.returncode}). See diagnostics above.")
