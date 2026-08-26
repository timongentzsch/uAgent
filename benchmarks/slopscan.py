#!/usr/bin/env python3
"""Find slop that already exists in the tree, not just in the last diff.

Centralising something is how slop appears: a helper moves out, the old body
stays behind under a return that can never be reached, a caller keeps its own
copy, a constant loses its last user, a doc keeps pointing at the file that was
renamed. None of that is in the diff that introduces the next change, so a
diff-only review never sees it.

Every check is a heuristic with a bias toward silence: it reports what a person
should look at, and says why. Findings are counted, and the count is what a
baseline can hold, so the tree can only get cleaner.

    python3 benchmarks/slopscan.py
    python3 benchmarks/slopscan.py --kind duplicate_block --verbose
    python3 benchmarks/slopscan.py --check          # fail if worse than baseline
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE_PATH = ROOT / "benchmarks" / "baselines" / "slop.json"

SOURCE_DIRS = ("src", "include")
ALL_CODE_DIRS = ("src", "include", "tests", "benchmarks")
DOC_FILES = ("README.md", "CONTRIBUTING.md", "CHANGELOG.md")

# A window shorter than this matches boilerplate; longer misses real copies.
DUPLICATE_WINDOW = 6
# Sentences below this length repeat innocently ("This is deliberate.").
SENTENCE_CHARS = 60


def code_files(dirs=SOURCE_DIRS, suffixes=(".cc", ".h")) -> list[Path]:
    return sorted(
        path
        for directory in dirs
        for path in (ROOT / directory).rglob("*")
        if path.suffix in suffixes and path.is_file()
    )


def finding(kind: str, where: str, detail: str) -> dict[str, str]:
    return {"kind": kind, "where": where, "detail": detail}


def unreachable_statements() -> list[dict[str, str]]:
    """Code after an unconditional return, the fossil a half-done move leaves."""
    out = []
    terminal = re.compile(r"^(\s*)(return\b[^;]*;|break;|continue;)\s*$")
    ignorable = re.compile(r"^\s*(//|/\*|\*|#|\}|$)")
    for path in code_files(ALL_CODE_DIRS, (".cc", ".h")):
        lines = path.read_text(errors="replace").splitlines()
        for index, line in enumerate(lines[:-1]):
            match = terminal.match(line)
            if not match:
                continue
            indent = match.group(1)
            following = lines[index + 1]
            if ignorable.match(following):
                continue
            # Same indent means the same block; deeper or shallower is a new
            # scope and reachable.
            if following.startswith(indent) and not following.startswith(indent + " "):
                out.append(
                    finding(
                        "unreachable",
                        f"{path.relative_to(ROOT)}:{index + 2}",
                        f"follows `{line.strip()}`",
                    )
                )
    return out


def unused_declarations() -> list[dict[str, str]]:
    """A header declares it, one place defines it, nobody calls it."""
    declaration = re.compile(r"^[A-Za-z_][\w:<>,&*\s]*?\b([A-Z]\w+)\s*\([^;]*\)\s*(const\s*)?;\s*$")
    corpus = {
        path: path.read_text(errors="replace") for path in code_files(ALL_CODE_DIRS, (".cc", ".h"))
    }
    out = []
    for path in code_files(("include",), (".h",)):
        for line in corpus[path].splitlines():
            match = declaration.match(line)
            if not match:
                continue
            name = match.group(1)
            uses = sum(len(re.findall(rf"\b{name}\b", text)) for text in corpus.values())
            # A name with no body is a deliberate link-time trap rather than
            # dead code: config_registry declares one so that naming an
            # unregistered setting fails to compile at the call site.
            defined = any(
                re.search(rf"\b{name}\s*\([^;]*\)\s*(const\s*)?\{{", text)
                for text in corpus.values()
            )
            # Declaration plus definition is two; a real caller makes three.
            if defined and uses <= 2:
                out.append(
                    finding(
                        "unused_declaration",
                        str(path.relative_to(ROOT)),
                        f"{name}() is declared and defined but never called",
                    )
                )
    return out


def normalise(line: str) -> str:
    return re.sub(r"\s+", " ", line.strip())


def duplicate_blocks() -> list[dict[str, str]]:
    """The same lines in two files: what a shared helper was supposed to remove."""
    windows: dict[str, list[tuple[Path, int]]] = collections.defaultdict(list)
    for path in code_files(ALL_CODE_DIRS, (".cc", ".h", ".py")):
        lines = path.read_text(errors="replace").splitlines()
        kept = [
            (number, normalise(line))
            for number, line in enumerate(lines, start=1)
            # Braces, comments and imports repeat everywhere and mean nothing.
            if len(normalise(line)) > 12 and not normalise(line).startswith(("//", "#", "*", "/*"))
        ]
        for start in range(len(kept) - DUPLICATE_WINDOW + 1):
            block = kept[start : start + DUPLICATE_WINDOW]
            key = "\n".join(text for _, text in block)
            windows[key].append((path, block[0][0]))
    out = []
    for key, places in windows.items():
        files = {path for path, _ in places}
        if len(files) < 2:
            continue
        where = ", ".join(f"{path.relative_to(ROOT)}:{line}" for path, line in sorted(places)[:3])
        out.append(
            finding(
                "duplicate_block",
                where,
                f"{DUPLICATE_WINDOW} identical lines starting `{key.splitlines()[0][:60]}`",
            )
        )
    return out


def stale_doc_paths() -> list[dict[str, str]]:
    """A doc pointing at a file that no longer exists.

    A path CMake installs under a different name is absent from a source
    checkout and still correct, so a line that says so is taken at its word.
    """
    reference = re.compile(r"`([\w./-]+\.(?:md|py|cc|h|json|sh|yml|txt))`")
    out = []
    docs = [ROOT / name for name in DOC_FILES if (ROOT / name).is_file()]
    docs += sorted((ROOT / "docs").rglob("*.md"))
    docs += sorted((ROOT / "skills").rglob("SKILL.md"))
    for doc in docs:
        text = doc.read_text(errors="replace")
        installed = {
            match
            for line in text.splitlines()
            if "when installed" in line
            for match in reference.findall(line)
        }
        for target in sorted(set(reference.findall(text)) - installed):
            candidates = [
                ROOT / target,
                doc.parent / target,
                ROOT / "docs" / target,
            ]
            if any(candidate.exists() for candidate in candidates):
                continue
            # Names that are generated, external or illustrative are not paths.
            if "/" not in target and not (ROOT / target).exists():
                continue
            out.append(finding("stale_doc_path", str(doc.relative_to(ROOT)), f"{target} not found"))
    return out


def duplicate_sentences() -> list[dict[str, str]]:
    """The same explanation maintained in two places drifts in one of them."""
    sentence = re.compile(rf"[A-Z][^.`|\n]{{{SENTENCE_CHARS},}}?\.")
    seen: dict[str, list[str]] = collections.defaultdict(list)
    docs = [ROOT / name for name in DOC_FILES if (ROOT / name).is_file()]
    docs += sorted((ROOT / "docs").rglob("*.md"))
    docs += sorted((ROOT / "skills").rglob("SKILL.md"))
    for doc in docs:
        for text in set(sentence.findall(doc.read_text(errors="replace"))):
            seen[normalise(text)].append(str(doc.relative_to(ROOT)))
    return [
        finding("duplicate_sentence", ", ".join(sorted(set(places))), text[:70])
        for text, places in seen.items()
        if len(set(places)) > 1
    ]


CHECKS = {
    "unreachable": unreachable_statements,
    "unused_declaration": unused_declarations,
    "duplicate_block": duplicate_blocks,
    "stale_doc_path": stale_doc_paths,
    "duplicate_sentence": duplicate_sentences,
}


def scan(kinds: list[str]) -> list[dict[str, str]]:
    return [item for kind in kinds for item in CHECKS[kind]()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", action="append", default=[], choices=sorted(CHECKS))
    parser.add_argument("--verbose", action="store_true", help="list every finding")
    parser.add_argument("--check", action="store_true", help="fail if worse than baseline")
    parser.add_argument("--update", action="store_true", help="rewrite the baseline")
    parser.add_argument("--json", type=Path, help="write findings as JSON")
    arguments = parser.parse_args()

    kinds = arguments.kind or sorted(CHECKS)
    findings = scan(kinds)
    counts = collections.Counter(item["kind"] for item in findings)
    for kind in kinds:
        print(f"{kind:<20} {counts[kind]:>4}")
    if arguments.verbose:
        for item in sorted(findings, key=lambda entry: (entry["kind"], entry["where"])):
            print(f"  {item['kind']:<20} {item['where']}\n      {item['detail']}")
    if arguments.json:
        arguments.json.write_text(json.dumps(findings, indent=2) + "\n", encoding="utf-8")

    baseline = (
        json.loads(BASELINE_PATH.read_text(encoding="utf-8")) if BASELINE_PATH.exists() else {}
    )
    if arguments.update:
        BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
        BASELINE_PATH.write_text(
            json.dumps({kind: counts[kind] for kind in sorted(CHECKS)}, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"baseline written: {BASELINE_PATH.relative_to(ROOT)}")
        return 0
    worse = [
        f"{kind} {baseline[kind]} → {counts[kind]}"
        for kind in kinds
        if kind in baseline and counts[kind] > baseline[kind]
    ]
    for regression in worse:
        print(f"REGRESSION: {regression}")
    return 1 if worse and arguments.check else 0


if __name__ == "__main__":
    sys.exit(main())
