# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""ViewLayer manager window launch orchestration."""

from __future__ import annotations

from blender_vfx_qt import ensure_runtime, qt_window_is_alive, show_unique_window


_window_cache = {"value": None}

def show_manager():
    bqt = ensure_runtime()
    cached_window = _window_cache.get("value")
    if qt_window_is_alive(cached_window):
        cached_window.refresh_from_blender()

    from .window import ViewLayerManagerWindow

    def factory():
        window = ViewLayerManagerWindow()
        bqt.add(window, unique=True)
        return window

    return show_unique_window(_window_cache, factory)
