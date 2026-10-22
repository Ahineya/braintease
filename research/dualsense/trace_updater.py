#!/usr/bin/env python3
"""Recover build-specific metadata references and dump static CIL from REA evidence.

Only reads files. Does not load a CLR, invoke reflection, run the application,
evaluate general CIL, or communicate with a controller. The XOR expressions
are algebraic reductions of four hash-pinned delegate factories in this build.
"""

import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
BASE = ROOT / "analysis/followup"
ASSEMBLY_SHA256 = "a5ccc41e04e960c867d479d3434e23d92044a731627f71646edb7b2b8dd4e409"
FACTORIES = {
    "0x06000a68": (3, "method", "5172d4ce93ffb79416d4d92afdc02b51b32367c7dfdce853cabeee018ed5b091"),
    "0x06000a69": (4, "method", "c4ea15112afd7403987c90ab73f93fb93aeb099c37769f4a06426561a8a7a684"),
    "0x06000a6a": (3, "constructor", "bf4bcaeaf79b2ec697a455e3bbbc6a5608913d8c3ade69240fc74e6eda384520"),
    "0x06000a6b": (4, "constructor", "637fdb488838baa78c59cdaf7f31b77d5b8c8ca06f8e628dcb00745363c72eb0"),
}
SELECTED = ["0x06000818", "0x0600081b", "0x0600081c", "0x0600081d",
            "0x06000820", "0x06000834", "0x06000844", "0x06000845", *FACTORIES]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def coordinates(arguments, kind):
    require(len(arguments) in [3, 4], "Unexpected factory argument count")
    require(kind in ["method", "constructor"], "Unknown factory kind")
    type_token = arguments[0] ^ 0x3A264FBA
    target = arguments[1] ^ (0x21F729E2 if kind == "method" else 0x2E0BC8AC)
    field = arguments[-1] ^ 0x4034CF00
    context = arguments[2] ^ 0x3A264FBA if len(arguments) == 4 else None
    return tuple(None if x is None else f"0x{x & 0xffffffff:08x}"
                 for x in [type_token, target, field, context])


class Trace:
    def __init__(self, evidence_path, binary_path):
        sys.path.insert(0, str(ROOT / "analysis/toolchain/python"))
        from dncil.cil.body.reader import read_method_body_from_bytes

        self.reader = read_method_body_from_bytes
        self.data = binary_path.read_bytes()
        require(hashlib.sha256(self.data).hexdigest() == ASSEMBLY_SHA256, "Unsupported assembly bytes")
        evidence = json.loads(evidence_path.read_text())
        require(evidence["operation"] == "inspect_managed_members", "Wrong REA operation")
        self.n = evidence["normalized_result"]
        require(evidence["subject"]["digest"]["sha256"] == ASSEMBLY_SHA256
                and self.n["artifact"]["sha256"] == ASSEMBLY_SHA256, "Evidence identity mismatch")
        self.methods = {x["token"]: x for x in self.n["methods"]}
        self.index = {x["token"]: x for key in ["types", "fields", "methods", "member_refs"]
                      for x in self.n[key]}
        self.strings = {x["token"]: x["value"] for x in self.n["user_strings"]["literals"]}
        self.proxies = {}
        self.rows = []

    def body(self, token):
        meta = self.methods[token]["body"]
        require(meta["status"] == "present", f"No CIL for {token}")
        offset = meta["file_offset"]
        body = self.reader(self.data[offset:])
        raw = self.data[offset + body.header_size:offset + body.header_size + body.code_size]
        require(body.code_size == meta["il_size"] and hashlib.sha256(raw).hexdigest() == meta["il_sha256"],
                f"Independent CIL decoding disagrees with REA for {token}")
        return body

    def recover(self):
        for token, (_, _, digest) in FACTORIES.items():
            self.body(token)
            require(self.methods[token]["body"]["il_sha256"] == digest, f"Unrecognized factory {token}")
        for method in self.methods.values():
            anchors = method["body"]["anchors"]
            if method["name"] != ".cctor" or not any(a.get("operand") in FACTORIES for a in anchors):
                continue
            instructions = self.body(method["token"]).instructions
            require(len(instructions) >= 3 and instructions[-2].opcode.name == "call"
                    and instructions[-1].opcode.name == "ret", "Unexpected proxy initializer")
            factory = f"0x{instructions[-2].operand.value:08x}"
            require(factory in FACTORIES, "Unexpected initializer target")
            count, kind, _ = FACTORIES[factory]
            require(len(instructions) == count + 2 and
                    all(x.opcode.name == "ldc.i4" for x in instructions[:count]),
                    "Initializer contains more than constant arguments")
            arguments = [x.operand for x in instructions[:count]]
            typ, target, field, context = coordinates(arguments, kind)
            require(typ == method["declaring_type_token"], "Decoded delegate type disagrees with metadata")
            require(target in self.index and field in self.index, "Decoded target/field is absent")
            require(self.index[field].get("declaring_type_token") == typ, "Field belongs to a different type")
            require(typ not in self.proxies, "Duplicate delegate type")
            item = self.index[target]
            name = (item.get("declaring_type", item.get("parent_name", "")) or "") + "." + item["name"]
            row = {"initializer": method["token"], "factory": factory, "kind": kind,
                   "type": typ, "field": field, "target": target, "generic_context_type": context,
                   "name": name, "initializer_il_sha256": method["body"]["il_sha256"]}
            self.proxies[typ] = row
            self.rows.append(row)
        require(len(self.rows) == 1719, f"Expected 1719 validated mappings, found {len(self.rows)}")
        return self.rows

    def label(self, token):
        if token in self.strings:
            return ascii(self.strings[token])
        item = self.index.get(token, {})
        proxy = self.proxies.get(item.get("declaring_type_token"))
        if proxy and item.get("name") not in [".ctor", ".cctor"]:
            return proxy["name"] + " [encoded metadata target]"
        return (item.get("declaring_type", item.get("parent_name", "")) or "") + "." + item.get("name", "")

    def listing(self, token):
        method, body = self.methods[token], self.body(token)
        lines = [f"{token} {self.label(token)}", json.dumps(method["signature"]),
                 f"IL SHA256 {method['body']['il_sha256']}"]
        for instruction in body.instructions:
            operand = instruction.operand
            label = self.label(f"0x{operand.value:08x}") if hasattr(operand, "value") else ""
            lines.append(f"{instruction.offset - body.header_size:04x} {instruction.opcode.name} {operand} {label}")
        return "\n".join(lines) + "\n"

    def edges(self):
        rows = []
        for edge in self.n["call_edges"]:
            item = self.index.get(edge["target_token"], {})
            proxy = self.proxies.get(item.get("declaring_type_token"))
            if proxy and item.get("name") not in [".ctor", ".cctor"]:
                rows.append({**edge, "encoded_metadata_target": proxy["target"], "target_label": proxy["name"]})
        return rows


def main():
    trace = Trace(BASE / "managed-members.json", BASE / "payload/Data1/playstationaccessories.dll")
    mappings = trace.recover()
    out = BASE / "trace"
    out.mkdir(exist_ok=True)
    result = {"assembly_sha256": ASSEMBLY_SHA256, "operation": "static metadata-coordinate recovery",
              "original_coverage": trace.n["coverage"], "mappings": mappings,
              "resolved_call_edges": trace.edges(),
              "limitations": ["Build-specific factory reductions; not a general deobfuscator.",
                              "Metadata targets do not establish execution or resolve virtual dispatch.",
                              "Only the four pinned factory families are recovered; other indirection remains unknown."]}
    (out / "metadata-references.json").write_text(json.dumps(result, indent=2) + "\n")
    for token in SELECTED:
        (out / (token + ".cil.txt")).write_text(trace.listing(token))
    print(f"Recovered {len(mappings)} metadata targets; saved original-byte-checked CIL under {out}")


if __name__ == "__main__":
    main()
