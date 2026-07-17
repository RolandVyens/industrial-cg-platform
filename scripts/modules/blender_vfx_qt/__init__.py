# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Fork-owned shared API wrapper for BQt runtime access."""

from __future__ import annotations

import importlib
import importlib.machinery
from importlib import metadata
import os
from pathlib import Path
import re
import shutil
import sys
import tomllib


_RUNTIME_EXTENSION_ID = "blender_vfx_qt_runtime"
_SYSTEM_REPOSITORY_MODULE = "system"
_RUNTIME_DLL_DIR_HANDLES: list[object] = []
_RUNTIME_PROVENANCE: tuple[str, Path, tuple[Path, ...]] | None = None
_RUNTIME_IMPORT_PREFIXES = (
    "bqt",
    "PySide6",
    "shiboken6",
    "blender_stylesheet",
)

_BQT_SAFE_ENV = {
    "BQT_DISABLE_WRAP": "1",
    "BQT_AUTO_ADD": "0",
    "BQT_DOCKABLE_WRAP": "0",
    "BQT_MANAGE_FOREGROUND": "1",
}

_RUNTIME_PACKAGE_WHEEL_PREFIXES = {
    "bqt": "bqt-",
    "PySide6": "pyside6-",
    "shiboken6": "shiboken6-",
}


def configure_bqt_environment() -> None:
    for key, value in _BQT_SAFE_ENV.items():
        os.environ.setdefault(key, value)


def qt_window_is_alive(widget) -> bool:
    if widget is None:
        return False
    try:
        # PySide raises RuntimeError when the wrapped C++ widget is already gone.
        widget.objectName()
    except RuntimeError:
        return False
    return True


def present_window(widget):
    widget.show()
    widget.raise_()
    widget.activateWindow()
    return widget


def show_unique_window(cache_ref: dict[str, object], factory):
    widget = cache_ref.get("value")
    if qt_window_is_alive(widget):
        return present_window(widget)
    widget = factory()
    cache_ref["value"] = widget
    return present_window(widget)


def _system_extension_repos(prefs):
    repos = []
    for repo in prefs.extensions.repos:
        if not repo.enabled:
            continue
        if repo.use_remote_url:
            continue
        if getattr(repo, "source", None) != 'SYSTEM':
            continue
        repos.append(repo)

    repos.sort(key=lambda repo: repo.module != _SYSTEM_REPOSITORY_MODULE)
    return repos


def _extension_manifest_path(repo, extension_id: str) -> Path | None:
    base_dir = Path(repo.directory)
    candidates = (
        base_dir / extension_id / "blender_manifest.toml",
        base_dir / repo.module / extension_id / "blender_manifest.toml",
    )
    for manifest_path in candidates:
        if manifest_path.is_file():
            return manifest_path
    return None


def _runtime_extension_info() -> tuple[str, Path] | None:
    try:
        import bpy
    except Exception:
        return None

    prefs = bpy.context.preferences
    for repo in _system_extension_repos(prefs):
        manifest_path = _extension_manifest_path(repo, _RUNTIME_EXTENSION_ID)
        if manifest_path is not None:
            return f"bl_ext.{repo.module}.{_RUNTIME_EXTENSION_ID}", manifest_path

    return None


def _import_extension_module(module_name: str) -> object:
    return importlib.import_module(module_name)


def _enable_runtime_extension() -> tuple[str, Path, tuple[Path, ...]]:
    runtime_info = _runtime_extension_info()
    if runtime_info is None:
        raise RuntimeError("BQt runtime extension is not available in this build")
    module_name, manifest_path = runtime_info

    import addon_utils

    err_str = ""

    def err_cb(ex):
        nonlocal err_str
        err_str = str(ex)

    module = addon_utils.enable(
        module_name,
        default_set=False,
        persistent=False,
        handle_error=err_cb,
    )
    if module is None:
        raise RuntimeError(err_str or "Failed to enable the bundled BQt runtime extension")

    package_paths = _runtime_extension_package_paths(module_name)
    return module_name, manifest_path, package_paths


def _runtime_extension_package_paths(module_name: str) -> tuple[Path, ...]:
    try:
        import bpy
    except Exception:
        return ()

    try:
        user_path = Path(bpy.utils.extension_path_user(module_name, path="", create=False))
        extension_store_root = user_path.parents[2]
    except (AttributeError, IndexError, TypeError, ValueError):
        return ()

    local_root = (extension_store_root / ".local").resolve()
    return tuple(
        path.resolve()
        for entry in sys.path
        if entry
        and (path := Path(entry)).is_dir()
        and path.resolve().is_relative_to(local_root)
    )


def _module_name_matches_prefix(module_name: str, prefixes: tuple[str, ...]) -> bool:
    return any(
        module_name == prefix or module_name.startswith(f"{prefix}.")
        for prefix in prefixes
    )


def _clear_runtime_import_state() -> None:
    raise RuntimeError(
        "In-process BQt runtime recovery is disabled because Qt modules and "
        "objects are process-global. Restart Blender before changing Qt runtimes."
    )


def _runtime_modules_already_loaded() -> tuple[str, ...]:
    return tuple(
        module_name
        for module_name in sys.modules
        if _module_name_matches_prefix(module_name, _RUNTIME_IMPORT_PREFIXES)
    )


def _runtime_package_versions(manifest_path: Path) -> dict[str, str]:
    with manifest_path.open("rb") as handle:
        manifest = tomllib.load(handle)

    versions: dict[str, str] = {}
    for wheel_path in manifest.get("wheels", ()):
        wheel_name = Path(str(wheel_path)).name.lower()
        for package_name, prefix in _RUNTIME_PACKAGE_WHEEL_PREFIXES.items():
            if not wheel_name.startswith(prefix):
                continue
            match = re.match(rf"^{re.escape(prefix)}([0-9][^-]*)-", wheel_name)
            if match is not None:
                versions[package_name] = match.group(1)
    missing = set(_RUNTIME_PACKAGE_WHEEL_PREFIXES) - set(versions)
    if missing:
        raise RuntimeError(f"Bundled BQt runtime manifest lacks versions for: {sorted(missing)}")
    return versions


def _module_is_within_roots(module, roots: tuple[Path, ...]) -> bool:
    module_file = getattr(module, "__file__", "")
    if not module_file:
        return False
    module_path = Path(module_file).resolve()
    return any(module_path.is_relative_to(root) for root in roots)


def _verify_runtime_package_provenance(
    runtime_info: tuple[str, Path, tuple[Path, ...]],
) -> None:
    _module_name, manifest_path, added_paths = runtime_info
    if not added_paths:
        raise RuntimeError("Bundled BQt runtime did not add a trusted package path")

    bqt = importlib.import_module("bqt")
    pyside6 = importlib.import_module("PySide6")
    shiboken6 = importlib.import_module("shiboken6")
    modules = {"bqt": bqt, "PySide6": pyside6, "shiboken6": shiboken6}
    versions = _runtime_package_versions(manifest_path)

    for package_name, module in modules.items():
        if not _module_is_within_roots(module, added_paths):
            raise RuntimeError(f"{package_name} was not loaded from the bundled BQt runtime")
        version = metadata.version(package_name)
        if version != versions[package_name]:
            raise RuntimeError(
                f"Bundled {package_name} version mismatch: expected {versions[package_name]}, got {version}"
            )


def _required_bqt_modules() -> tuple[str, ...]:
    modules = ["bqt.manager"]
    if sys.platform == "win32":
        modules.append("bqt.blender_applications.win32_blender_application")
    elif sys.platform == "darwin":
        modules.append("bqt.blender_applications.darwin_blender_application")
    return tuple(modules)


def _import_runtime_packages() -> tuple[object | None, object | None, Exception | None]:
    try:
        import bqt
        from PySide6.QtWidgets import QApplication

        for module_name in _required_bqt_modules():
            importlib.import_module(module_name)
        if not callable(getattr(bqt, "register", None)):
            raise RuntimeError("BQt runtime does not expose register()")
        if not callable(getattr(bqt, "add", None)):
            raise RuntimeError("BQt runtime does not expose add()")
    except Exception as ex:
        return None, None, ex
    return bqt, QApplication, None


def _debug_extension_suffixes_only() -> bool:
    suffixes = importlib.machinery.EXTENSION_SUFFIXES
    return bool(suffixes) and all(suffix.startswith("_d") for suffix in suffixes)


def _package_roots(package_name: str) -> list[Path]:
    roots: list[Path] = []
    seen: set[Path] = set()
    for entry in sys.path:
        if not entry:
            continue
        package_root = Path(entry) / package_name
        if not package_root.is_dir():
            continue
        resolved_root = package_root.resolve()
        if resolved_root in seen:
            continue
        seen.add(resolved_root)
        roots.append(resolved_root)
    return roots


def _link_or_copy_file(source: Path, target: Path) -> None:
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def _ensure_runtime_dll_directories() -> None:
    if os.name != "nt" or not hasattr(os, "add_dll_directory"):
        return

    known_paths = {Path(handle.path) for handle in _RUNTIME_DLL_DIR_HANDLES}
    for package_name in ("shiboken6", "PySide6"):
        for package_root in _package_roots(package_name):
            if package_root in known_paths:
                continue
            _RUNTIME_DLL_DIR_HANDLES.append(os.add_dll_directory(str(package_root)))
            known_paths.add(package_root)


def _ensure_debug_extension_aliases() -> None:
    if not _debug_extension_suffixes_only():
        return

    for package_name in ("shiboken6", "PySide6"):
        for package_root in _package_roots(package_name):
            for source_path in package_root.glob("*.pyd"):
                if source_path.stem.endswith("_d"):
                    continue
                alias_path = source_path.with_name(f"{source_path.stem}_d.pyd")
                if alias_path.exists():
                    continue
                _link_or_copy_file(source_path, alias_path)


def ensure_bqt_runtime():
    global _RUNTIME_PROVENANCE

    configure_bqt_environment()

    preloaded_modules = _runtime_modules_already_loaded()
    if preloaded_modules:
        if _RUNTIME_PROVENANCE is None:
            raise RuntimeError(
                "BQt runtime modules are already loaded outside the bundled runtime: "
                + ", ".join(preloaded_modules)
            )
        runtime_info = _RUNTIME_PROVENANCE
    else:
        runtime_info = _enable_runtime_extension()
        _ensure_runtime_dll_directories()
        _ensure_debug_extension_aliases()
    bqt, qapplication, import_error = _import_runtime_packages()
    if bqt is None:
        error_suffix = f" ({import_error})" if import_error is not None else ""
        raise RuntimeError(
            "BQt runtime is not available. Ensure blender_vfx_qt_runtime is "
            "installed and bundled with the current build."
            f"{error_suffix}"
        )
    _verify_runtime_package_provenance(runtime_info)
    _RUNTIME_PROVENANCE = runtime_info

    try:
        bqt.register()
    except NotImplementedError as ex:
        raise RuntimeError(str(ex)) from ex

    app = qapplication.instance()
    if app is None:
        raise RuntimeError("BQt could not create a QApplication instance")
    if os.getenv("BQT_DISABLE_WRAP") != "1" and not hasattr(app, "blender_widget"):
        raise RuntimeError("A QApplication exists, but it is not managed by BQt")

    return bqt


def ensure_runtime(report_fn=None):
    try:
        return ensure_bqt_runtime()
    except Exception as ex:
        if report_fn is not None:
            report_fn({'ERROR'}, str(ex))
        raise


__all__ = (
    "configure_bqt_environment",
    "qt_window_is_alive",
    "present_window",
    "show_unique_window",
    "ensure_bqt_runtime",
    "ensure_runtime",
)
