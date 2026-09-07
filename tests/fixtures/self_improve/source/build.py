#!/usr/bin/env python3
"""Stand-in build step: produce the runnable artifact this tree describes."""

import pathlib
import shutil

ROOT = pathlib.Path(__file__).resolve().parent
BUILD = ROOT / "build"
BUILD.mkdir(exist_ok=True)
artifact = BUILD / "uagent"
shutil.copyfile(ROOT / "src" / "agent.py", artifact)
artifact.chmod(0o755)
print(f"built {artifact}")
