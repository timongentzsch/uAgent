"""Test-only helpers for µAgent's private memory layout."""

from __future__ import annotations

from pathlib import Path


def _safe_component(value: str) -> str:
    value = "".join(
        character if (character.isascii() and character.isalnum()) or character in "-_" else "_"
        for character in value
    )
    return (value or "unnamed")[:80]


def _repository_identity(cwd: Path) -> tuple[Path, Path]:
    root = next((path for path in (cwd, *cwd.parents) if (path / ".git").exists()), cwd)
    dot_git = root / ".git"
    if dot_git.is_dir():
        return dot_git.resolve(), root
    if not dot_git.is_file():
        return root.resolve(), root

    first_line = dot_git.read_text(encoding="utf-8").splitlines()[0].strip()
    if not first_line.startswith("gitdir:"):
        return root.resolve(), root
    git_dir = Path(first_line.removeprefix("gitdir:").strip())
    if not git_dir.is_absolute():
        git_dir = root / git_dir
    git_dir = git_dir.resolve()
    common = git_dir / "commondir"
    if not common.is_file():
        return git_dir, root
    common_dir = Path(common.read_text(encoding="utf-8").splitlines()[0].strip())
    if not common_dir.is_absolute():
        common_dir = git_dir / common_dir
    return common_dir.resolve(), root


def _workspace_id(path: Path) -> str:
    value = 1469598103934665603
    for byte in str(path).encode():
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def global_memory_dir(home: Path) -> Path:
    return home / ".uagent" / "memory" / "global"


def project_memory_dir(home: Path, cwd: Path) -> Path:
    identity, root = _repository_identity(cwd.resolve())
    label = identity.parent if identity.name == ".git" else root
    return (
        home
        / ".uagent"
        / "memory"
        / "projects"
        / (f"{_safe_component(label.name)}-{_workspace_id(identity)}")
    )
