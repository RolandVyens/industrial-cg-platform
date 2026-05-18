# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Thin bootstrap around the fork-owned Qt runtime wrapper."""

from __future__ import annotations

from blender_vfx_qt import ensure_bqt_runtime


def ensure_runtime():
    return ensure_bqt_runtime()
