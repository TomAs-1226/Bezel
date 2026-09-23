"""Which repo paths the tablet may see or touch. Every code endpoint and every patch edit goes through
resolve(); anything outside the repo, inside .git, or on the deny list is refused with 403."""
from __future__ import annotations

import re
from fnmatch import fnmatchcase
from pathlib import Path

from .state import LinkError

# Matched, lower-cased, against every path component.
DENY_GLOBS: tuple[str, ...] = (
    "*.env", ".env*", "*secret*", "*.pem", "*.key",
    "*.p12", "*.pfx", "*.jks", "*.keystore", "id_rsa*", "id_ed25519*", "id_ecdsa*",
)
# Directories never listed, read or written, at any depth (build output, Gradle's cache, Eclipse's bin/).
DENY_DIRS: frozenset[str] = frozenset({".git", "build", ".gradle", "bin"})


def denied(parts: tuple[str, ...] | list[str]) -> str | None:
    """Why a repo-relative path is off limits, or None."""
    for part in parts:
        low = part.lower()
        if low == ".git":
            return ".git is off limits"
        if low in DENY_DIRS:
            return f"{part}/ is deny-listed"
        for glob in DENY_GLOBS:
            if fnmatchcase(low, glob):
                return f"{part} is deny-listed ({glob})"
    return None


def clean(rel: object) -> str:
    """A normalised repo-relative path ("" is the root). Refuses absolute paths and '..'."""
    if rel is None:
        return ""
    if not isinstance(rel, str):
        raise LinkError(400, "path must be a string")
    if "\x00" in rel:
        raise LinkError(400, "path contains NUL")
    rel = rel.replace("\\", "/").strip()
    if rel.startswith("/") or re.match(r"^[A-Za-z]:", rel):
        raise LinkError(403, f"{rel}: absolute paths are refused")
    parts = [p for p in rel.split("/") if p not in ("", ".")]
    if ".." in parts:
        raise LinkError(403, f"{rel}: path escapes the repo")
    return "/".join(parts)


def resolve(root: Path, rel: object, *, must_exist: bool = False) -> tuple[str, Path]:
    """(clean relative path, real absolute path). `root` must already be resolved.

    Symlinks are followed and the target must still be inside the repo and not deny-listed."""
    rel = clean(rel)
    parts = tuple(rel.split("/")) if rel else ()
    reason = denied(parts)
    if reason:
        raise LinkError(403, f"{rel}: {reason}")
    candidate = root / rel if rel else root
    real = candidate.resolve()
    try:
        real_rel = real.relative_to(root)
    except ValueError:
        raise LinkError(403, f"{rel}: resolves outside the repo") from None
    reason = denied(real_rel.parts)
    if reason:
        raise LinkError(403, f"{rel}: {reason}")
    if must_exist and not candidate.exists():
        raise LinkError(404, f"{rel or '.'}: no such file")
    return rel, real


def target_ok(root: Path, path: Path) -> bool:
    """For a symlink met while walking: its target is inside the repo and not deny-listed."""
    try:
        real = path.resolve().relative_to(root)
    except (ValueError, OSError):
        return False
    return denied(real.parts) is None


def inside(root: Path, path: Path) -> bool:
    try:
        path.resolve().relative_to(root)
        return True
    except ValueError:
        return False
