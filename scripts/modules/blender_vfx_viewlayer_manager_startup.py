# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Pure startup policy for the bundled ViewLayer Manager extension."""

from __future__ import annotations

from enum import Enum
from pathlib import Path
from typing import NamedTuple


_REPOSITORY_RETRY_DELAYS = (0.25, 0.5, 1.0, 1.0)


class PrewarmFailure(Enum):
    REPOSITORY_NOT_READY = "repository_not_ready"
    PERMANENT = "permanent"


class ExtensionSelection(NamedTuple):
    module_name: str
    manifest_path: Path


def repository_retry_delay(attempt: int) -> float | None:
    """Return the delay after a failed repository probe, or stop retrying."""
    index = attempt - 1
    if index < 0 or index >= len(_REPOSITORY_RETRY_DELAYS):
        return None
    return _REPOSITORY_RETRY_DELAYS[index]


def retry_delay(failure: PrewarmFailure, *, attempt: int) -> float | None:
    if failure != PrewarmFailure.REPOSITORY_NOT_READY:
        return None
    return repository_retry_delay(attempt)


def _manifest_candidates(repo, extension_id: str) -> tuple[Path, Path]:
    base_dir = Path(repo.directory)
    return (
        base_dir / extension_id / "blender_manifest.toml",
        base_dir / repo.module / extension_id / "blender_manifest.toml",
    )


def select_extension(
    repos,
    extension_id: str,
    *,
    preferred_module: str = "system",
) -> ExtensionSelection | None:
    """Select an installed local system extension without importing it."""
    candidates = [
        repo
        for repo in repos
        if repo.enabled
        and not repo.use_remote_url
        and getattr(repo, "source", None) == "SYSTEM"
    ]
    candidates.sort(key=lambda repo: repo.module != preferred_module)

    for repo in candidates:
        for manifest_path in _manifest_candidates(repo, extension_id):
            if manifest_path.is_file():
                return ExtensionSelection(
                    module_name=f"bl_ext.{repo.module}.{extension_id}",
                    manifest_path=manifest_path,
                )
    return None
