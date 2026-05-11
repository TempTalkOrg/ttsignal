#!/usr/bin/env python3
"""
Scan built artifacts for sensitive build-host info that would leak the
developer's username, home directory, or absolute repo location.

Default scan targets are file extensions commonly produced by this repo
(``.node``, ``.so``, ``.dylib``, ``.dll``, ``.a``, ``.lib``). When given
a file, it is scanned regardless of extension.

Patterns checked (case-insensitive, ASCII + UTF-16LE):

* ``X:\\Users\\<name>...``    Windows user directory absolute path
* ``/Users/<name>...``        macOS user directory absolute path
* ``/home/<name>...``         Linux user directory absolute path
* repository absolute path    (both ``/`` and ``\\`` separator variants)
* current OS username         (``USER`` / ``USERNAME`` / ``LOGNAME``,
                              only when length >= 3)

Exit codes:
    0  no hits in any scanned file (or no scannable files found)
    1  one or more hits
    2  bad invocation
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_EXTS = (".node", ".so", ".dylib", ".dll", ".a", ".lib")


def _collect_username_patterns() -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for var in ("USER", "USERNAME", "LOGNAME"):
        val = os.environ.get(var, "").strip()
        if len(val) < 3 or val.lower() in seen:
            continue
        seen.add(val.lower())
        out.append(rf"\b{re.escape(val)}\b")
    return out


def _build_patterns() -> list[tuple[str, re.Pattern[str]]]:
    repo_str = str(REPO_ROOT)
    repo_variants = {
        repo_str,
        repo_str.replace("\\", "/"),
        repo_str.replace("/", "\\"),
    }
    raw: list[tuple[str, str]] = [
        ("win-userdir", r"(?i)[a-z]:\\users\\[^\\:\x00\r\n\"<>|*?]+"),
        ("mac-userdir", r"(?i)/Users/[A-Za-z0-9._-]+"),
        ("linux-homedir", r"(?i)/home/[A-Za-z0-9._-]+"),
    ]
    for v in repo_variants:
        raw.append(("repo-abs-path", re.escape(v)))
    for u in _collect_username_patterns():
        raw.append(("os-username", u))
    return [(name, re.compile(pat)) for name, pat in raw]


PATTERNS = _build_patterns()


def _iter_targets(roots: Iterable[Path], exts: tuple[str, ...]) -> Iterable[Path]:
    exts_lc = tuple(e.lower() for e in exts)
    for root in roots:
        if not root.exists():
            print(f"WARN: missing path: {root}", file=sys.stderr)
            continue
        if root.is_file():
            yield root
            continue
        for child in root.rglob("*"):
            if child.is_file() and child.suffix.lower() in exts_lc:
                yield child


def _scan_file(path: Path) -> list[tuple[str, str, str]]:
    raw = path.read_bytes()
    latin = raw.decode("latin1", errors="ignore")
    u16 = raw.decode("utf-16-le", errors="ignore")
    hits: list[tuple[str, str, str]] = []
    for name, pat in PATTERNS:
        for enc, hay in (("ascii", latin), ("utf16le", u16)):
            m = pat.search(hay)
            if m:
                sample = m.group(0)
                if len(sample) > 120:
                    sample = sample[:120] + "..."
                hits.append((name, enc, sample))
                break
    return hits


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Verify built artifacts contain no developer paths or usernames.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "paths",
        nargs="+",
        help="Files or directories to scan. Directories recurse for --ext matches.",
    )
    parser.add_argument(
        "--ext",
        action="append",
        default=None,
        metavar="EXT",
        help="File extension to include when scanning a directory "
        "(may be repeated; default: .node .so .dylib .dll .a .lib)",
    )
    parser.add_argument(
        "--allow-empty",
        action="store_true",
        help="Exit 0 if no scannable files are found (default: exit 0 with WARN).",
    )
    args = parser.parse_args(argv[1:])

    exts = tuple(args.ext) if args.ext else DEFAULT_EXTS
    targets = sorted({Path(p).resolve(): None for p in args.paths}.keys())
    files = list(_iter_targets(targets, exts))

    if not files:
        msg = (
            f"WARN: no files matching {','.join(exts)} found under "
            f"{', '.join(str(t) for t in targets)}"
        )
        print(msg, file=sys.stderr)
        return 0 if args.allow_empty else 0

    print("============================================================")
    print(f"[verify] scanning {len(files)} file(s) for sensitive info")
    print(f"[verify] repo root : {REPO_ROOT}")
    if any(name == "os-username" for name, _ in PATTERNS):
        usernames = [
            os.environ.get(v, "").strip()
            for v in ("USER", "USERNAME", "LOGNAME")
            if len(os.environ.get(v, "").strip()) >= 3
        ]
        print(f"[verify] usernames : {', '.join(sorted(set(usernames)))}")
    print("============================================================")

    failed = False
    for f in files:
        try:
            hits = _scan_file(f)
        except OSError as exc:
            print(f"[ERR ] {f}: {exc}", file=sys.stderr)
            failed = True
            continue
        rel = f
        try:
            rel = f.relative_to(REPO_ROOT)
        except ValueError:
            pass
        if hits:
            failed = True
            print(f"[FAIL] {rel}")
            for name, enc, sample in hits:
                print(f"       - {name:<14s} enc={enc:<7s} sample={sample!r}")
        else:
            print(f"[ OK ] {rel}")

    print("============================================================")
    if failed:
        print("[verify] FAILED: sensitive info detected in artifact(s) above.")
        print("[verify] Inspect the listed strings and adjust the build flags")
        print("[verify] (e.g. -ffile-prefix-map / /d1trimfile) before shipping.")
        return 1
    print("[verify] OK: no sensitive info detected.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
