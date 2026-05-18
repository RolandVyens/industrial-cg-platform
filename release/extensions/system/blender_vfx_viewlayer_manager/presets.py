# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""User-level pass preset helpers for Blender VFX ViewLayer Manager."""

from __future__ import annotations

import json
import os
import re
from datetime import datetime, timezone
from typing import Any, Mapping

import bpy


PRESET_SCHEMA_VERSION = 2
PRESET_SCHEMA_VERSION_LEGACY = 1
PRESET_KIND = "viewlayer_pass_toggles"
PRESET_EXTENSION = ".json"
PRESET_SUBDIR = os.path.join("presets", "pass_toggles")
PRESET_NAME_MAX_LENGTH = 80

_PRESET_NAME_SAFE_RE = re.compile(r"[^A-Za-z0-9._-]+")
ENGINE_BLENDER_EEVEE = "BLENDER_EEVEE"
ENGINE_CYCLES = "CYCLES"

SHARED_VIEW_LAYER_PASS_PROPS: tuple[str, ...] = (
    "use_deep",
    "use_pass_combined",
    "use_pass_z",
    "use_pass_mist",
    "use_pass_normal",
    "use_pass_position",
    "use_pass_vector",
    "use_pass_grease_pencil",
    "use_pass_diffuse_direct",
    "use_pass_glossy_direct",
    "use_pass_emit",
    "use_pass_environment",
    "use_pass_ambient_occlusion",
    "use_pass_diffuse_color",
    "use_pass_glossy_color",
    "use_pass_cryptomatte_object",
    "use_pass_cryptomatte_material",
    "use_pass_cryptomatte_asset",
    "use_pass_cryptomatte_accurate",
)
SHARED_VIEW_LAYER_VALUE_PROPS: tuple[str, ...] = (
    "pass_cryptomatte_depth",
)

EEVEE_ENGINE_VIEW_LAYER_PASS_PROPS: tuple[str, ...] = (
    "use_pass_shadow",
)

EEVEE_ENGINE_TARGET_SPECS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("view_layer", (
        *EEVEE_ENGINE_VIEW_LAYER_PASS_PROPS,
    )),
    ("view_layer.eevee", (
        "use_pass_volume_direct",
        "use_pass_transparent",
    )),
)

CYCLES_ENGINE_VIEW_LAYER_PASS_PROPS: tuple[str, ...] = (
    "use_pass_uv",
    "use_pass_object_index",
    "use_pass_material_index",
    "use_pass_diffuse_indirect",
    "use_pass_glossy_indirect",
    "use_pass_transmission_direct",
    "use_pass_transmission_indirect",
    "use_pass_transmission_color",
)

CYCLES_ENGINE_TARGET_SPECS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("view_layer", (
        *CYCLES_ENGINE_VIEW_LAYER_PASS_PROPS,
    )),
    ("view_layer.cycles", (
        "use_pass_volume_direct",
        "use_pass_volume_indirect",
        "use_pass_shadow_catcher",
    )),
)

CYCLES_LIGHT_PASS_AOV_MASTER_PROP = "use_lightgroup_light_pass_aovs"
CYCLES_LIGHT_PASS_AOV_PROPS: tuple[str, ...] = tuple(
    f"use_lightgroup_light_pass_aov_{lobe}_{suffix}"
    for lobe in ("diffuse", "glossy", "transmission", "volume")
    for suffix in ("all", "combined", "direct", "indirect")
)

_CYCLES_LIGHT_PASS_ALLOWED = set(CYCLES_LIGHT_PASS_AOV_PROPS)

SHARED_TARGET_SPECS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("view_layer", (*SHARED_VIEW_LAYER_PASS_PROPS, *SHARED_VIEW_LAYER_VALUE_PROPS)),
)

_SHARED_ALLOWED_MAP = {target: set(props) for target, props in SHARED_TARGET_SPECS}
_EEVEE_ENGINE_ALLOWED_MAP = {target: set(props) for target, props in EEVEE_ENGINE_TARGET_SPECS}
_CYCLES_ENGINE_ALLOWED_MAP = {target: set(props) for target, props in CYCLES_ENGINE_TARGET_SPECS}

# Backward-compatible maps used to normalize schema v1 preset files.
EEVEE_PASS_TARGET_SPECS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("view_layer", (
        *SHARED_VIEW_LAYER_PASS_PROPS,
        *SHARED_VIEW_LAYER_VALUE_PROPS,
        *EEVEE_ENGINE_VIEW_LAYER_PASS_PROPS,
    )),
    ("view_layer.eevee", (
        "use_pass_volume_direct",
        "use_pass_transparent",
    )),
)
CYCLES_PASS_TARGET_SPECS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("view_layer", (
        *SHARED_VIEW_LAYER_PASS_PROPS,
        *SHARED_VIEW_LAYER_VALUE_PROPS,
        *CYCLES_ENGINE_VIEW_LAYER_PASS_PROPS,
    )),
    ("view_layer.cycles", (
        "use_pass_volume_direct",
        "use_pass_volume_indirect",
        "use_pass_shadow_catcher",
    )),
)
_LEGACY_EEVEE_ALLOWED_MAP = {target: set(props) for target, props in EEVEE_PASS_TARGET_SPECS}
_LEGACY_CYCLES_ALLOWED_MAP = {target: set(props) for target, props in CYCLES_PASS_TARGET_SPECS}

_DEFAULT_PACKAGE = "bl_ext.system.blender_vfx_viewlayer_manager"


def _extension_package() -> str:
    package = __package__
    if package:
        return package
    return _DEFAULT_PACKAGE


def _extension_module_ids() -> tuple[str, str]:
    package = _extension_package()
    parts = package.split(".")
    if len(parts) >= 3 and parts[0] == "bl_ext":
        return parts[1], parts[2]
    return "system", "blender_vfx_viewlayer_manager"


def _resolve_target(view_layer, target_path: str):
    if target_path == "view_layer":
        return view_layer
    if target_path == "view_layer.eevee":
        return getattr(view_layer, "eevee", None)
    if target_path == "view_layer.cycles":
        return getattr(view_layer, "cycles", None)
    raise KeyError(target_path)


def _as_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    return False


def _normalize_target_value(prop_name: str, value: Any) -> Any:
    if prop_name in SHARED_VIEW_LAYER_VALUE_PROPS:
        if isinstance(value, (int, float)):
            return int(value)
        return 0
    return _as_bool(value)


def _collect_target_props(view_layer, target_specs) -> dict[str, dict[str, Any]]:
    engine_data: dict[str, dict[str, Any]] = {}
    for target_path, prop_names in target_specs:
        owner = _resolve_target(view_layer, target_path)
        if owner is None:
            continue
        prop_data: dict[str, Any] = {}
        for prop_name in prop_names:
            if not hasattr(owner, prop_name):
                continue
            prop_data[prop_name] = _normalize_target_value(prop_name, getattr(owner, prop_name))
        if prop_data:
            engine_data[target_path] = prop_data
    return engine_data


def _normalize_target_props(data: Any, allowed_map: Mapping[str, set[str]]) -> dict[str, dict[str, Any]]:
    if not isinstance(data, Mapping):
        return {}

    normalized: dict[str, dict[str, Any]] = {}
    for target_path, allowed_props in allowed_map.items():
        raw_target = data.get(target_path)
        if not isinstance(raw_target, Mapping):
            continue

        target_values: dict[str, Any] = {}
        for prop_name in allowed_props:
            if prop_name not in raw_target:
                continue
            target_values[prop_name] = _normalize_target_value(prop_name, raw_target[prop_name])
        if target_values:
            normalized[target_path] = target_values
    return normalized


def _normalize_cycles_light_pass_props(data: Any) -> dict[str, Any]:
    if not isinstance(data, Mapping):
        return {
            CYCLES_LIGHT_PASS_AOV_MASTER_PROP: False,
            "aovs": {},
        }

    aovs = data.get("aovs")
    normalized_aovs: dict[str, bool] = {}
    if isinstance(aovs, Mapping):
        for prop_name in _CYCLES_LIGHT_PASS_ALLOWED:
            if prop_name not in aovs:
                continue
            normalized_aovs[prop_name] = _as_bool(aovs[prop_name])

    return {
        CYCLES_LIGHT_PASS_AOV_MASTER_PROP: _as_bool(data.get(CYCLES_LIGHT_PASS_AOV_MASTER_PROP, False)),
        "aovs": normalized_aovs,
    }


def _normalize_engine_block(data: Any, *, allowed_map: Mapping[str, set[str]], include_cycles_light_pass: bool = False) -> dict[str, Any]:
    normalized = _normalize_target_props(data, allowed_map)
    if include_cycles_light_pass:
        cycles_light_pass_data = data.get("cycles_light_pass") if isinstance(data, Mapping) else None
        normalized["cycles_light_pass"] = _normalize_cycles_light_pass_props(cycles_light_pass_data)
    return normalized


def _normalize_schema_version(data: Mapping[str, Any]) -> int:
    if "schema_version" in data:
        return int(data["schema_version"])
    if "shared" in data or "engines" in data:
        return PRESET_SCHEMA_VERSION
    return PRESET_SCHEMA_VERSION_LEGACY


def _shared_view_layer_from_legacy(
    *,
    legacy_eevee: Mapping[str, Any],
    legacy_cycles: Mapping[str, Any],
) -> dict[str, bool]:
    legacy_eevee_view_layer = legacy_eevee.get("view_layer")
    legacy_cycles_view_layer = legacy_cycles.get("view_layer")

    shared_values: dict[str, Any] = {}
    for prop_name in (*SHARED_VIEW_LAYER_PASS_PROPS, *SHARED_VIEW_LAYER_VALUE_PROPS):
        if isinstance(legacy_cycles_view_layer, Mapping) and prop_name in legacy_cycles_view_layer:
            shared_values[prop_name] = _normalize_target_value(prop_name, legacy_cycles_view_layer[prop_name])
            continue
        if isinstance(legacy_eevee_view_layer, Mapping) and prop_name in legacy_eevee_view_layer:
            shared_values[prop_name] = _normalize_target_value(prop_name, legacy_eevee_view_layer[prop_name])
    return shared_values


def _normalize_preset_data_v1(data: Mapping[str, Any], *, preset_name: str, saved_at_utc: str) -> dict[str, Any]:
    legacy_eevee = _normalize_target_props(data.get("eevee"), _LEGACY_EEVEE_ALLOWED_MAP)
    legacy_cycles = _normalize_target_props(data.get("cycles"), _LEGACY_CYCLES_ALLOWED_MAP)

    shared_view_layer = _shared_view_layer_from_legacy(
        legacy_eevee=legacy_eevee,
        legacy_cycles=legacy_cycles,
    )
    eevee_engine = _normalize_target_props(legacy_eevee, _EEVEE_ENGINE_ALLOWED_MAP)
    cycles_engine = _normalize_engine_block(
        legacy_cycles,
        allowed_map=_CYCLES_ENGINE_ALLOWED_MAP,
    )
    cycles_engine["cycles_light_pass"] = _normalize_cycles_light_pass_props(data.get("cycles_light_pass"))

    return {
        "schema_version": PRESET_SCHEMA_VERSION,
        "kind": PRESET_KIND,
        "name": preset_name,
        "saved_at_utc": saved_at_utc,
        "shared": {"view_layer": shared_view_layer} if shared_view_layer else {},
        "engines": {
            ENGINE_BLENDER_EEVEE: eevee_engine,
            ENGINE_CYCLES: cycles_engine,
        },
    }


def _normalize_preset_data_v2(data: Mapping[str, Any], *, preset_name: str, saved_at_utc: str) -> dict[str, Any]:
    shared = _normalize_target_props(data.get("shared"), _SHARED_ALLOWED_MAP)
    engines_data = data.get("engines")
    if not isinstance(engines_data, Mapping):
        engines_data = {}

    eevee_engine = _normalize_engine_block(
        engines_data.get(ENGINE_BLENDER_EEVEE),
        allowed_map=_EEVEE_ENGINE_ALLOWED_MAP,
    )
    cycles_engine = _normalize_engine_block(
        engines_data.get(ENGINE_CYCLES),
        allowed_map=_CYCLES_ENGINE_ALLOWED_MAP,
        include_cycles_light_pass=True,
    )

    return {
        "schema_version": PRESET_SCHEMA_VERSION,
        "kind": PRESET_KIND,
        "name": preset_name,
        "saved_at_utc": saved_at_utc,
        "shared": shared,
        "engines": {
            ENGINE_BLENDER_EEVEE: eevee_engine,
            ENGINE_CYCLES: cycles_engine,
        },
    }


def normalize_preset_name(preset_name: str) -> str:
    name = _PRESET_NAME_SAFE_RE.sub("_", preset_name.strip())
    name = name.strip("._-")
    if not name:
        raise ValueError("Preset name cannot be empty")
    if len(name) > PRESET_NAME_MAX_LENGTH:
        name = name[:PRESET_NAME_MAX_LENGTH]
    return name


def get_preset_directory(*, create: bool = False) -> str:
    package = _extension_package()
    extension_path_user = getattr(bpy.utils, "extension_path_user", None)
    if extension_path_user is not None:
        try:
            directory = extension_path_user(package, path=PRESET_SUBDIR, create=create)
        except Exception:
            directory = ""
        if directory:
            return directory

    base_extensions_dir = bpy.utils.user_resource("EXTENSIONS", create=create)
    if not base_extensions_dir:
        return ""

    repo_module, pkg_idname = _extension_module_ids()
    directory = os.path.join(
        base_extensions_dir,
        ".user",
        repo_module,
        pkg_idname,
        PRESET_SUBDIR,
    )
    if create and not os.path.isdir(directory):
        os.makedirs(directory, exist_ok=True)
    return directory


def get_preset_filepath(preset_name: str, *, create_dir: bool = False) -> str:
    directory = get_preset_directory(create=create_dir)
    if not directory:
        raise RuntimeError("Unable to resolve user preset directory")
    safe_name = normalize_preset_name(preset_name)
    return os.path.join(directory, f"{safe_name}{PRESET_EXTENSION}")


def list_pass_presets() -> list[str]:
    directory = get_preset_directory(create=False)
    if not directory or not os.path.isdir(directory):
        return []

    preset_names: list[str] = []
    for entry_name in os.listdir(directory):
        entry_path = os.path.join(directory, entry_name)
        if not os.path.isfile(entry_path):
            continue
        stem, suffix = os.path.splitext(entry_name)
        if suffix.lower() != PRESET_EXTENSION:
            continue
        preset_names.append(stem)
    preset_names.sort(key=str.casefold)
    return preset_names


def _normalize_preset_data(data: Any, *, fallback_name: str = "") -> dict[str, Any]:
    if not isinstance(data, Mapping):
        raise ValueError("Preset payload must be a dictionary")

    schema_version = _normalize_schema_version(data)
    if schema_version not in {PRESET_SCHEMA_VERSION_LEGACY, PRESET_SCHEMA_VERSION}:
        raise ValueError(
            f"Unsupported preset schema version: {schema_version} "
            f"(expected {PRESET_SCHEMA_VERSION_LEGACY} or {PRESET_SCHEMA_VERSION})"
        )

    kind = data.get("kind", PRESET_KIND)
    if kind != PRESET_KIND:
        raise ValueError(f"Unsupported preset kind: {kind}")

    preset_name = str(data.get("name", fallback_name)).strip()
    if not preset_name:
        preset_name = fallback_name or "Unnamed"
    saved_at_utc = str(data.get("saved_at_utc", ""))

    if schema_version == PRESET_SCHEMA_VERSION_LEGACY:
        return _normalize_preset_data_v1(data, preset_name=preset_name, saved_at_utc=saved_at_utc)
    return _normalize_preset_data_v2(data, preset_name=preset_name, saved_at_utc=saved_at_utc)


def collect_pass_preset(view_layer, *, preset_name: str) -> dict[str, Any]:
    if view_layer is None:
        raise ValueError("ViewLayer cannot be None")

    cycles_owner = getattr(view_layer, "cycles", None)
    cycles_light_pass_values = {
        prop_name: bool(getattr(cycles_owner, prop_name))
        for prop_name in CYCLES_LIGHT_PASS_AOV_PROPS
        if cycles_owner is not None and hasattr(cycles_owner, prop_name)
    }

    raw_data = {
        "schema_version": PRESET_SCHEMA_VERSION,
        "kind": PRESET_KIND,
        "name": preset_name,
        "saved_at_utc": datetime.now(timezone.utc).isoformat(),
        "shared": _collect_target_props(view_layer, SHARED_TARGET_SPECS),
        "engines": {
            ENGINE_BLENDER_EEVEE: _collect_target_props(view_layer, EEVEE_ENGINE_TARGET_SPECS),
            ENGINE_CYCLES: {
                **_collect_target_props(view_layer, CYCLES_ENGINE_TARGET_SPECS),
                "cycles_light_pass": {
                    CYCLES_LIGHT_PASS_AOV_MASTER_PROP: (
                        bool(getattr(cycles_owner, CYCLES_LIGHT_PASS_AOV_MASTER_PROP))
                        if cycles_owner is not None and hasattr(cycles_owner, CYCLES_LIGHT_PASS_AOV_MASTER_PROP)
                        else False
                    ),
                    "aovs": cycles_light_pass_values,
                },
            },
        },
    }
    return _normalize_preset_data(raw_data, fallback_name=preset_name)


def save_pass_preset(view_layer, preset_name: str) -> str:
    filepath = get_preset_filepath(preset_name, create_dir=True)
    preset_data = collect_pass_preset(view_layer, preset_name=preset_name)
    temporary_path = f"{filepath}.tmp"
    with open(temporary_path, "w", encoding="utf-8") as handle:
        json.dump(preset_data, handle, indent=2, ensure_ascii=False, sort_keys=True)
        handle.write("\n")
    os.replace(temporary_path, filepath)
    return filepath


def load_pass_preset(preset_name: str) -> dict[str, Any]:
    filepath = get_preset_filepath(preset_name, create_dir=False)
    if not os.path.isfile(filepath):
        raise FileNotFoundError(filepath)
    with open(filepath, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    return _normalize_preset_data(data, fallback_name=preset_name)


def delete_pass_preset(preset_name: str) -> bool:
    filepath = get_preset_filepath(preset_name, create_dir=False)
    if not os.path.isfile(filepath):
        return False
    os.remove(filepath)
    return True


def apply_pass_preset(view_layer, preset_data: Mapping[str, Any]) -> bool:
    if view_layer is None:
        raise ValueError("ViewLayer cannot be None")

    normalized = _normalize_preset_data(preset_data)
    changed = False
    engine = getattr(bpy.context.scene.render, "engine", "")

    def _apply_target_block(block_data: Mapping[str, Any]) -> None:
        nonlocal changed
        for target_path, target_data in block_data.items():
            if target_path == "cycles_light_pass":
                continue
            owner = _resolve_target(view_layer, target_path)
            if owner is None or not isinstance(target_data, Mapping):
                continue
            for prop_name, value in target_data.items():
                if not hasattr(owner, prop_name):
                    continue
                if getattr(owner, prop_name) != value:
                    setattr(owner, prop_name, value)
                    changed = True

    shared_block = normalized["shared"]
    engines_block = normalized["engines"]

    if engine == ENGINE_BLENDER_EEVEE:
        _apply_target_block(shared_block)
        _apply_target_block(engines_block[ENGINE_BLENDER_EEVEE])
        return changed

    if engine != ENGINE_CYCLES:
        return changed

    _apply_target_block(shared_block)
    cycles_engine_block = engines_block[ENGINE_CYCLES]
    _apply_target_block(cycles_engine_block)

    cycles_owner = getattr(view_layer, "cycles", None)
    if cycles_owner is None:
        return changed

    cycles_light_pass = cycles_engine_block["cycles_light_pass"]
    master_value = cycles_light_pass[CYCLES_LIGHT_PASS_AOV_MASTER_PROP]
    if hasattr(cycles_owner, CYCLES_LIGHT_PASS_AOV_MASTER_PROP):
        if getattr(cycles_owner, CYCLES_LIGHT_PASS_AOV_MASTER_PROP) != master_value:
            setattr(cycles_owner, CYCLES_LIGHT_PASS_AOV_MASTER_PROP, master_value)
            changed = True

    for prop_name, value in cycles_light_pass["aovs"].items():
        if not hasattr(cycles_owner, prop_name):
            continue
        if getattr(cycles_owner, prop_name) != value:
            setattr(cycles_owner, prop_name, value)
            changed = True

    return changed


def apply_named_pass_preset(view_layer, preset_name: str) -> bool:
    preset_data = load_pass_preset(preset_name)
    return apply_pass_preset(view_layer, preset_data)


__all__ = (
    "PRESET_SCHEMA_VERSION",
    "PRESET_KIND",
    "PRESET_EXTENSION",
    "PRESET_SUBDIR",
    "EEVEE_PASS_TARGET_SPECS",
    "CYCLES_PASS_TARGET_SPECS",
    "CYCLES_LIGHT_PASS_AOV_MASTER_PROP",
    "CYCLES_LIGHT_PASS_AOV_PROPS",
    "normalize_preset_name",
    "get_preset_directory",
    "get_preset_filepath",
    "list_pass_presets",
    "collect_pass_preset",
    "save_pass_preset",
    "load_pass_preset",
    "delete_pass_preset",
    "apply_pass_preset",
    "apply_named_pass_preset",
)
