#!/usr/bin/env python3
"""Write THIRD-PARTY-NOTICES/phonon-core.txt: the license texts of every Rust crate that
phonon-core links statically into the plugin, on macOS and Windows, from Cargo.lock.

Only crates that end up in the binary are listed: normal dependencies, not build scripts,
dev-dependencies or procedural macros. Run it after changing Cargo.toml or Cargo.lock:

    python3 phonon-core/third_party_notices.py
"""

import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
OUTPUT = ROOT.parent / "THIRD-PARTY-NOTICES" / "phonon-core.txt"
TARGETS = ["aarch64-apple-darwin", "x86_64-apple-darwin", "x86_64-pc-windows-msvc"]
LICENSE_PREFIXES = ("license", "licence", "copying", "notice", "copyright", "unlicense")


def linked_packages(target):
    meta = json.loads(subprocess.check_output(
        ["cargo", "metadata", "--format-version", "1", "--locked",
         "--filter-platform", target, "--manifest-path", str(ROOT / "Cargo.toml")]))
    packages = {p["id"]: p for p in meta["packages"]}
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    root = meta["resolve"]["root"]
    seen, stack = set(), [root]
    while stack:
        pid = stack.pop()
        if pid in seen:
            continue
        seen.add(pid)
        for dep in nodes[pid]["deps"]:
            if any(k["kind"] is None for k in dep["dep_kinds"]):
                stack.append(dep["pkg"])
    seen.discard(root)
    result = {}
    for pid in seen:
        p = packages[pid]
        if any("proc-macro" in t["kind"] for t in p["targets"]):
            continue
        result[(p["name"], p["version"])] = p
    return result


def license_texts(package):
    folder = pathlib.Path(package["manifest_path"]).parent
    files = sorted(f for f in folder.iterdir()
                   if f.is_file() and f.name.lower().startswith(LICENSE_PREFIXES))
    if package.get("license_file"):
        extra = folder / package["license_file"]
        if extra.is_file() and extra not in files:
            files.append(extra)
    return [(f.name, f.read_text(encoding="utf-8", errors="replace").strip()) for f in files]


MIT_TEXT = """Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE."""


def main():
    packages = {}
    for target in TARGETS:
        packages.update(linked_packages(target))
    out = [
        "Third-party notices for phonon-core",
        "",
        "The 4D plugin links the following Rust crates statically, as resolved by",
        "phonon-core/Cargo.lock. Where a crate offers a choice of licenses, it is used",
        "under MIT where offered, otherwise Apache-2.0, otherwise the license listed.",
        "",
    ]
    printed = {}
    for (name, version) in sorted(packages):
        p = packages[(name, version)]
        out.append("=" * 78)
        out.append(f"{name} {version}")
        out.append(f"License: {p.get('license') or 'see below'}")
        if p.get("repository"):
            out.append(f"Source: {p['repository']}")
        out.append("=" * 78)
        texts = license_texts(p)
        if not texts and "MIT" in (p.get("license") or ""):
            authors = ", ".join(p.get("authors") or []) or f"the {name} authors"
            texts = [("MIT (the crate ships no license file)", f"Copyright (c) {authors}\n\n{MIT_TEXT}")]
        if not texts:
            out.append(f"(no license file in the published crate; license: {p.get('license')})")
        for fname, text in texts:
            out.append(f"--- {fname} ---")
            if text in printed:
                out.append(f"(same text as {printed[text]})")
            else:
                printed[text] = f"{name} {version}, {fname}"
                out.append(text)
        out.append("")
    OUTPUT.parent.mkdir(exist_ok=True)
    OUTPUT.write_text("\n".join(out).rstrip("\n") + "\n", encoding="utf-8", newline="\n")
    print(f"{len(packages)} crates -> {OUTPUT}")


if __name__ == "__main__":
    sys.exit(main())
