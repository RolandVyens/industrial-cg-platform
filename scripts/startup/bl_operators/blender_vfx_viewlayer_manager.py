# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Fork-side bridge for opening the bundled BQt ViewLayer Manager."""

from __future__ import annotations

import importlib
import sys
import time

import bpy
from bpy.app.handlers import persistent
from bpy.app.translations import pgettext_rpt as rpt_
from bpy.types import Operator

from blender_vfx_viewlayer_manager_startup import (
    PrewarmFailure,
    retry_delay,
    select_extension,
)


EXTENSION_ID = "blender_vfx_viewlayer_manager"
SYSTEM_REPOSITORY_MODULE = "system"
_startup_prewarm_attempts = 0
_startup_prewarm_complete = False
_startup_prewarm_timer_queued = False


def _extension_module_name() -> str | None:
    selection = select_extension(
        bpy.context.preferences.extensions.repos,
        EXTENSION_ID,
        preferred_module=SYSTEM_REPOSITORY_MODULE,
    )
    return selection.module_name if selection is not None else None


def _import_extension_module(module_name: str) -> object:
    return importlib.import_module(module_name)


def _startup_prewarm_supported() -> bool:
    return sys.platform == "win32" and not bpy.app.background


def _enable_extension(*, report_fn=None, raise_on_error: bool = False) -> object | None:
    import addon_utils

    module_name = _extension_module_name()
    if module_name is None:
        if report_fn is not None:
            report_fn({'WARNING'}, rpt_("BQt ViewLayer Manager extension is not available in this build"))
        return None

    err_str = ""

    def err_cb(ex):
        nonlocal err_str
        err_str = str(ex)

    _, was_enabled = addon_utils.check(module_name)
    if was_enabled:
        try:
            return _import_extension_module(module_name)
        except Exception:
            pass

    module = addon_utils.enable(
        module_name,
        default_set=False,
        persistent=False,
        handle_error=err_cb,
    )
    if module is None:
        message = err_str or rpt_("Failed to enable BQt ViewLayer Manager extension")
        if report_fn is not None:
            report_fn({'ERROR'}, message)
        if raise_on_error:
            raise RuntimeError(message)
    elif module is not None and report_fn is not None and not was_enabled:
        report_fn({'INFO'}, rpt_("Enabled the bundled BQt ViewLayer Manager for this session"))
    return module


def _show_extension_window(report_fn=None) -> bool:
    module = _enable_extension(report_fn=report_fn)
    if module is None:
        return False

    try:
        if hasattr(module, "show_manager"):
            module.show_manager()
        else:
            raise RuntimeError(rpt_("Extension module does not expose show_manager()"))
    except Exception as ex:
        if report_fn is not None:
            report_fn({'ERROR'}, str(ex))
        return False

    return True


def _startup_prewarm_timer() -> float | None:
    global _startup_prewarm_attempts
    global _startup_prewarm_complete
    global _startup_prewarm_timer_queued

    _startup_prewarm_timer_queued = False
    if _startup_prewarm_complete or not _startup_prewarm_supported():
        return None

    _startup_prewarm_attempts += 1
    started_at = time.perf_counter()
    if _extension_module_name() is None:
        delay = retry_delay(
            PrewarmFailure.REPOSITORY_NOT_READY,
            attempt=_startup_prewarm_attempts,
        )
        if delay is not None:
            _startup_prewarm_timer_queued = True
            return delay
        print(
            "[BQt] startup prewarm stopped: bundled extension repository "
            f"was not ready after {_startup_prewarm_attempts} attempts"
        )
        return None

    try:
        _enable_extension(raise_on_error=True)
        from blender_vfx_qt import ensure_bqt_runtime

        ensure_bqt_runtime()
    except Exception as ex:
        elapsed = time.perf_counter() - started_at
        print(
            "[BQt] startup prewarm stopped after permanent failure "
            f"({elapsed:.3f}s): {ex}"
        )
        return None

    _startup_prewarm_complete = True
    elapsed = time.perf_counter() - started_at
    print(f"[BQt] startup prewarm ready ({elapsed:.3f}s)")
    return None


def _queue_startup_prewarm() -> None:
    global _startup_prewarm_attempts
    global _startup_prewarm_timer_queued

    if _startup_prewarm_timer_queued and not bpy.app.timers.is_registered(_startup_prewarm_timer):
        _startup_prewarm_timer_queued = False

    if (
        not _startup_prewarm_supported()
        or _startup_prewarm_complete
        or _startup_prewarm_timer_queued
    ):
        return

    _startup_prewarm_attempts = 0
    _startup_prewarm_timer_queued = True
    bpy.app.timers.register(_startup_prewarm_timer, first_interval=0.0, persistent=False)


@persistent
def _load_post_queue_startup_prewarm(_filepath) -> None:
    global _startup_prewarm_timer_queued

    if not bpy.app.timers.is_registered(_startup_prewarm_timer):
        _startup_prewarm_timer_queued = False
    _queue_startup_prewarm()


class WM_OT_blender_vfx_viewlayer_manager_show(Operator):
    bl_idname = "wm.blender_vfx_viewlayer_manager_show"
    bl_label = "Open ViewLayer Manager"
    bl_description = "Open the BQt ViewLayer Manager"

    @classmethod
    def poll(cls, _context):
        return sys.platform == "win32"

    def execute(self, _context):
        if sys.platform != "win32":
            self.report({'WARNING'}, rpt_("BQt ViewLayer Manager is only bundled for Windows builds"))
            return {'CANCELLED'}
        if _show_extension_window(report_fn=self.report):
            return {'FINISHED'}
        return {'CANCELLED'}


classes = (
    WM_OT_blender_vfx_viewlayer_manager_show,
)


def register() -> None:
    if _load_post_queue_startup_prewarm not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_load_post_queue_startup_prewarm)
    _queue_startup_prewarm()
    return None


def unregister() -> None:
    global _startup_prewarm_attempts
    global _startup_prewarm_complete
    global _startup_prewarm_timer_queued

    if bpy.app.timers.is_registered(_startup_prewarm_timer):
        bpy.app.timers.unregister(_startup_prewarm_timer)
    if _load_post_queue_startup_prewarm in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_load_post_queue_startup_prewarm)
    _startup_prewarm_attempts = 0
    _startup_prewarm_complete = False
    _startup_prewarm_timer_queued = False
    return None
