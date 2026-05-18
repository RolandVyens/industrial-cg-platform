# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Shared BQt runtime extension entrypoint."""

from __future__ import annotations

from .bootstrap import ensure_runtime


def register() -> None:
    return None


def unregister() -> None:
    return None
