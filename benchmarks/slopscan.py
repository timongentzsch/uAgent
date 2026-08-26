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

A count above the baseline exits non-zero. A gate that has to be asked to fail
is not a gate: the one caller who forgets the flag gets a silent pass, which is
the only failure mode that matters here.

    python3 benchmarks/slopscan.py
    python3 benchmarks/slopscan.py --kind duplicate_block --verbose
    python3 benchmarks/slopscan.py --update         # accept the current counts
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
# The fixture tree plants an instance of every check deliberately, so scanning
# it as ordinary source would report those forever. Compared as leading path
# components rather than as a substring, so a future tests/fixtures/slop_old
# is not swallowed by the same rule.
EXCLUDED_PREFIX = ("tests", "fixtures", "slop")
FIXTURE_ROOT = ROOT.joinpath(*EXCLUDED_PREFIX)

# A window shorter than this matches boilerplate; longer misses real copies.
DUPLICATE_WINDOW = 6
# Sentences below this length repeat innocently ("This is deliberate.").
SENTENCE_CHARS = 60
# A blank line ends a run of prose, and so does the start of a list item,
# heading, table row or quote.
BLOCK_START = re.compile(r"^\s*(?:[-*+]\s|\d+[.)]\s|#{1,6}\s|\||>)")
FENCE = re.compile(r"^\s*(?:```|~~~)")


def excluded(path: Path, root: Path) -> bool:
    if not path.is_relative_to(root):
        return False
    return path.relative_to(root).parts[: len(EXCLUDED_PREFIX)] == EXCLUDED_PREFIX


def code_files(dirs=SOURCE_DIRS, suffixes=(".cc", ".h"), root: Path = ROOT) -> list[Path]:
    return sorted(
        path
        for directory in dirs
        for path in (root / directory).rglob("*")
        if path.suffix in suffixes and path.is_file() and not excluded(path, root)
    )


def doc_files(root: Path = ROOT) -> list[Path]:
    """Every prose file the doc checks read: root docs, docs/, and skills."""
    docs = [root / name for name in DOC_FILES if (root / name).is_file()]
    docs += sorted((root / "docs").rglob("*.md"))
    docs += sorted((root / "skills").rglob("SKILL.md"))
    return docs


def finding(kind: str, where: str, detail: str) -> dict[str, str]:
    return {"kind": kind, "where": where, "detail": detail}


def unreachable_statements(root: Path = ROOT) -> list[dict[str, str]]:
    """Code after an unconditional return, the fossil a half-done move leaves."""
    out = []
    terminal = re.compile(r"^(\s*)(return\b[^;]*;|break;|continue;)\s*$")
    ignorable = re.compile(r"^\s*(//|/\*|\*|#|\}|$)")
    for path in code_files(ALL_CODE_DIRS, (".cc", ".h"), root):
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
                        f"{path.relative_to(root)}:{index + 2}",
                        f"follows `{line.strip()}`",
                    )
                )
    return out


def unused_declarations(root: Path = ROOT) -> list[dict[str, str]]:
    """A header declares it, one place defines it, nobody calls it."""
    declaration = re.compile(r"^[A-Za-z_][\w:<>,&*\s]*?\b([A-Z]\w+)\s*\([^;]*\)\s*(const\s*)?;\s*$")
    corpus = {
        path: path.read_text(errors="replace")
        for path in code_files(ALL_CODE_DIRS, (".cc", ".h"), root)
    }
    out = []
    for path in code_files(("include",), (".h",), root):
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
                        str(path.relative_to(root)),
                        f"{name}() is declared and defined but never called",
                    )
                )
    return out


def normalise(line: str) -> str:
    return re.sub(r"\s+", " ", line.strip())


def duplicate_blocks(root: Path = ROOT) -> list[dict[str, str]]:
    """The same lines in two files: what a shared helper was supposed to remove."""
    windows: dict[str, list[tuple[Path, int]]] = collections.defaultdict(list)
    for path in code_files(ALL_CODE_DIRS, (".cc", ".h", ".py"), root):
        lines = path.read_text(errors="replace").splitlines()
        kept = [
            (number, normalise(line))
            for number, line in enumerate(lines, start=1)
            # Braces, comments and imports repeat everywhere and mean nothing.
            # `#include` is caught by the comment prefixes; a python import is
            # not, and two files that import the same six modules are not a
            # helper someone failed to extract.
            if len(normalise(line)) > 12
            and not normalise(line).startswith(("//", "#", "*", "/*", "import ", "from "))
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
        where = ", ".join(f"{path.relative_to(root)}:{line}" for path, line in sorted(places)[:3])
        out.append(
            finding(
                "duplicate_block",
                where,
                f"{DUPLICATE_WINDOW} identical lines starting `{key.splitlines()[0][:60]}`",
            )
        )
    return out


def stale_doc_paths(root: Path = ROOT) -> list[dict[str, str]]:
    """A doc pointing at a file that no longer exists.

    A path CMake installs under a different name is absent from a source
    checkout and still correct, so a line that says so is taken at its word.
    """
    reference = re.compile(r"`([\w./-]+\.(?:md|py|cc|h|json|sh|yml|txt))`")
    out = []
    for doc in doc_files(root):
        text = doc.read_text(errors="replace")
        installed = {
            match
            for line in text.splitlines()
            if "when installed" in line
            for match in reference.findall(line)
        }
        for target in sorted(set(reference.findall(text)) - installed):
            candidates = [
                root / target,
                doc.parent / target,
                root / "docs" / target,
            ]
            if any(candidate.exists() for candidate in candidates):
                continue
            # Names that are generated, external or illustrative are not paths.
            if "/" not in target and not (root / target).exists():
                continue
            out.append(finding("stale_doc_path", str(doc.relative_to(root)), f"{target} not found"))
    return out


def prose_blocks(text: str) -> list[str]:
    """Runs of continuous prose, each joined onto one line.

    Prose here wraps at about 79 columns, so a sentence worth reporting is
    almost never on a single line and matching line by line sees nearly
    nothing. Joining has its own failure: run a bullet list together and the
    result contains sentences straddling two bullets that nobody wrote. So a
    block ends at a blank line or at any line that starts a new one, and
    fenced code is dropped rather than read as prose.
    """
    blocks: list[str] = []
    current: list[str] = []
    fenced = False
    for line in text.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            line = ""
        if fenced or not line.strip() or BLOCK_START.match(line):
            if current:
                blocks.append(normalise(" ".join(current)))
            current = []
        if fenced or not line.strip():
            continue
        current.append(line)
    if current:
        blocks.append(normalise(" ".join(current)))
    return blocks


def duplicate_sentences(root: Path = ROOT) -> list[dict[str, str]]:
    """The same explanation maintained in two places drifts in one of them."""
    sentence = re.compile(rf"[A-Z][^.`|]{{{SENTENCE_CHARS},}}?\.")
    seen: dict[str, list[str]] = collections.defaultdict(list)
    for doc in doc_files(root):
        for block in prose_blocks(doc.read_text(errors="replace")):
            for text in set(sentence.findall(block)):
                seen[normalise(text)].append(str(doc.relative_to(root)))
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


def scan(kinds: list[str], root: Path = ROOT) -> list[dict[str, str]]:
    return [item for kind in kinds for item in CHECKS[kind](root)]


def self_test() -> int:
    """Prove the checks still fire.

    Every count against the real tree is zero, and a check that silently
    returned nothing at all would look exactly the same. Each check runs
    against a tree carrying a planted instance of the thing it looks for, so a
    broken regex fails here instead of passing quietly for months.

    The assertion is "at least one", not an exact count. Exact counts would
    gate the build on incidental properties of the fixtures — how many lines a
    sliding window happens to share — which a reformat can change without
    touching a check, and the cheapest way out of that failure is to edit the
    fixture until it passes.
    """
    counts = collections.Counter(item["kind"] for item in scan(sorted(CHECKS), FIXTURE_ROOT))
    failures = []
    for kind in sorted(CHECKS):
        actual = counts[kind]
        print(f"{kind:<20} {actual:>4} (want at least 1)")
        if actual < 1:
            failures.append(f"{kind} found nothing in the fixtures")
    for failure in failures:
        print(f"SELF-TEST FAILED: {failure}")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", action="append", default=[], choices=sorted(CHECKS))
    parser.add_argument("--verbose", action="store_true", help="list every finding")
    parser.add_argument("--update", action="store_true", help="rewrite the baseline")
    parser.add_argument(
        "--self-test", action="store_true", help="check the checks against fixtures"
    )
    parser.add_argument("--json", type=Path, help="write findings as JSON")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()

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
    return 1 if worse else 0


if __name__ == "__main__":
    sys.exit(main())
