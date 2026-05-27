# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Fork-side bridge for opening the bundled BQt ViewLayer Manager."""

from __future__ import annotations

import importlib
import sys
from pathlib import Path

import bpy
from bpy.app.handlers import persistent
from bpy.app.translations import pgettext_rpt as rpt_
from bpy.types import Operator


EXTENSION_ID = "blender_vfx_viewlayer_manager"
SYSTEM_REPOSITORY_MODULE = "system"
_STARTUP_PREWARM_INTERVAL_SECONDS = 0.25
_STARTUP_PREWARM_MAX_ATTEMPTS = 20
_startup_prewarm_attempts = 0
_startup_prewarm_complete = False
_startup_prewarm_timer_queued = False


def _system_extension_repos():
    repos = []
    prefs = bpy.context.preferences
    for repo in prefs.extensions.repos:
        if not repo.enabled:
            continue
        if repo.use_remote_url:
            continue
        if getattr(repo, "source", None) != 'SYSTEM':
            continue
        repos.append(repo)

    repos.sort(key=lambda repo: repo.module != SYSTEM_REPOSITORY_MODULE)
    return repos


def _extension_manifest_path(repo) -> Path | None:
    base_dir = Path(repo.directory)
    candidates = (
        base_dir / EXTENSION_ID / "blender_manifest.toml",
        base_dir / repo.module / EXTENSION_ID / "blender_manifest.toml",
    )
    for manifest_path in candidates:
        if manifest_path.is_file():
            return manifest_path
    return None


def _extension_module_name() -> str | None:
    for repo in _system_extension_repos():
        manifest_path = _extension_manifest_path(repo)
        if manifest_path is not None:
            return f"bl_ext.{repo.module}.{EXTENSION_ID}"
    return None


def _import_extension_module(module_name: str) -> object:
    return importlib.import_module(module_name)


def _startup_prewarm_supported() -> bool:
    return sys.platform == "win32" and not bpy.app.background


def _enable_extension(*, report_fn=None) -> object | None:
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
    if module is None and report_fn is not None:
        report_fn({'ERROR'}, err_str or rpt_("Failed to enable BQt ViewLayer Manager extension"))
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
    module = _enable_extension()
    if module is None:
        if _startup_prewarm_attempts < _STARTUP_PREWARM_MAX_ATTEMPTS:
            _startup_prewarm_timer_queued = True
            return _STARTUP_PREWARM_INTERVAL_SECONDS
        return None

    try:
        from blender_vfx_qt import ensure_bqt_runtime

        ensure_bqt_runtime()
    except Exception as ex:
        print(
            "[BQt] startup prewarm attempt "
            f"{_startup_prewarm_attempts} failed: {ex}"
        )
        if _startup_prewarm_attempts < _STARTUP_PREWARM_MAX_ATTEMPTS:
            _startup_prewarm_timer_queued = True
            return _STARTUP_PREWARM_INTERVAL_SECONDS
        return None

    _startup_prewarm_complete = True
    print("[BQt] startup prewarm ready")
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
