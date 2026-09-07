#!/usr/bin/env python3
"""The subject tree's pre-existing gate: it must survive every candidate."""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REQUIRED = ("src/agent.py", "build.py")


def main():
    missing = [name for name in REQUIRED if not (ROOT / name).is_file()]
    if missing:
        print(f"missing required files: {missing}")
        return 1
    if (ROOT / "broken.marker").exists():
        print("the candidate broke this gate")
        return 1
    print("gate ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
