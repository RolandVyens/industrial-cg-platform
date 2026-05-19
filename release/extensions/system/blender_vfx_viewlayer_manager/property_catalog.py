# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Version-keyed pass property catalog for the ViewLayer manager."""

from __future__ import annotations

import json
import os
from typing import Any, Iterable, Mapping, Sequence

import bpy


TARGET_VIEW_LAYER = "view_layer"
TARGET_VIEW_LAYER_EEVEE = "view_layer.eevee"
TARGET_VIEW_LAYER_CYCLES = "view_layer.cycles"

_TARGET_PATHS = (
    TARGET_VIEW_LAYER,
    TARGET_VIEW_LAYER_EEVEE,
    TARGET_VIEW_LAYER_CYCLES,
)

_ADDITIONAL_TITLES = {
    TARGET_VIEW_LAYER: "Additional",
    TARGET_VIEW_LAYER_EEVEE: "Additional (Eevee)",
    TARGET_VIEW_LAYER_CYCLES: "Additional (Cycles)",
}

_ENGINE_TARGET_ORDER = {
    "eevee_specs": (TARGET_VIEW_LAYER, TARGET_VIEW_LAYER_EEVEE),
    "cycles_specs": (TARGET_VIEW_LAYER, TARGET_VIEW_LAYER_CYCLES),
}

_FALLBACK_TYPE_NAMES = {
    TARGET_VIEW_LAYER: ("ViewLayer",),
    TARGET_VIEW_LAYER_EEVEE: ("ViewLayerEEVEE",),
    TARGET_VIEW_LAYER_CYCLES: ("CyclesRenderLayerSettings",),
}

_DEFAULT_PACKAGE = "bl_ext.system.blender_vfx_viewlayer_manager"
_CATALOG_FORMAT_VERSION = 1
_CATALOG_SUBDIR = "cache"
_CATALOG_FILENAME = "view_layer_pass_catalog.json"

_CATALOG_CACHE_BY_VERSION: dict[tuple[int, int, int], dict[str, object]] = {}


def _version_key() -> tuple[int, int, int]:
    version = tuple(int(part) for part in bpy.app.version[:3])
    if len(version) < 3:
        version = version + (0,) * (3 - len(version))
    return (version[0], version[1], version[2])


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
    return ("system", "blender_vfx_viewlayer_manager")


def _catalog_directory(*, create: bool = False) -> str:
    package = _extension_package()
    extension_path_user = getattr(bpy.utils, "extension_path_user", None)
    if extension_path_user is not None:
        try:
            directory = extension_path_user(package, path=_CATALOG_SUBDIR, create=create)
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
        _CATALOG_SUBDIR,
    )
    if create and not os.path.isdir(directory):
        os.makedirs(directory, exist_ok=True)
    return directory


def _catalog_filepath(*, create: bool = False) -> str:
    directory = _catalog_directory(create=create)
    if not directory:
        return ""
    return os.path.join(directory, _CATALOG_FILENAME)


def _sample_view_layer():
    scene = getattr(bpy.context, "scene", None)
    if scene is None:
        return None
    view_layers = getattr(scene, "view_layers", None)
    if not view_layers:
        return None
    try:
        return view_layers[0]
    except Exception:
        for view_layer in view_layers:
            return view_layer
    return None


def _resolve_target_owner(view_layer, target_path: str):
    if target_path == TARGET_VIEW_LAYER:
        return view_layer
    if target_path == TARGET_VIEW_LAYER_EEVEE:
        return getattr(view_layer, "eevee", None) if view_layer is not None else None
    if target_path == TARGET_VIEW_LAYER_CYCLES:
        return getattr(view_layer, "cycles", None) if view_layer is not None else None
    return None


def _fallback_target_owner(target_path: str):
    for type_name in _FALLBACK_TYPE_NAMES.get(target_path, ()):
        owner = getattr(bpy.types, type_name, None)
        if owner is not None:
            return owner
    return None


def _collect_target_owners() -> dict[str, object | None]:
    view_layer = _sample_view_layer()
    owners: dict[str, object | None] = {
        target_path: _resolve_target_owner(view_layer, target_path)
        for target_path in _TARGET_PATHS
    }
    for target_path, owner in owners.items():
        if owner is None:
            owners[target_path] = _fallback_target_owner(target_path)
    return owners


def _humanize_property_name(identifier: str) -> str:
    raw = identifier.removeprefix("use_pass_")
    if not raw:
        return identifier
    words: list[str] = []
    for token in raw.split("_"):
        if not token:
            continue
        if len(token) <= 2:
            words.append(token.upper())
        else:
            words.append(token.capitalize())
    return " ".join(words) if words else identifier


def _collect_pass_like_boolean_properties(owner) -> dict[str, str]:
    if owner is None:
        return {}
    bl_rna = getattr(owner, "bl_rna", None)
    rna_properties = getattr(bl_rna, "properties", None)
    if rna_properties is None:
        return {}

    pass_props: dict[str, str] = {}
    for rna_prop in rna_properties:
        identifier = getattr(rna_prop, "identifier", "")
        if not identifier.startswith("use_pass_"):
            continue
        if getattr(rna_prop, "type", "") != 'BOOLEAN':
            continue
        if getattr(rna_prop, "is_readonly", False):
            continue
        label = getattr(rna_prop, "name", "") or _humanize_property_name(identifier)
        pass_props[identifier] = label
    return pass_props


def _build_known_props_by_target(
    eevee_specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
    cycles_specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
    known_props_by_target: Mapping[str, Iterable[str]] | None,
) -> dict[str, set[str]]:
    known: dict[str, set[str]] = {target_path: set() for target_path in _TARGET_PATHS}
    for specs in (eevee_specs, cycles_specs):
        for _section_title, target_path, props in specs:
            known.setdefault(target_path, set()).update(prop_name for prop_name, _label in props)
    if known_props_by_target:
        for target_path, prop_names in known_props_by_target.items():
            known.setdefault(target_path, set()).update(prop_names)
    return known


def _filter_specs_by_availability(
    specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
    available_props_by_target: Mapping[str, Mapping[str, str]],
) -> tuple[tuple[str, str, tuple[tuple[str, str], ...]], ...]:
    filtered_sections: list[tuple[str, str, tuple[tuple[str, str], ...]]] = []
    for section_title, target_path, props in specs:
        available = available_props_by_target.get(target_path, {})
        filtered_props = tuple(
            (prop_name, label)
            for prop_name, label in props
            if prop_name in available
        )
        if filtered_props:
            filtered_sections.append((section_title, target_path, filtered_props))
    return tuple(filtered_sections)


def _build_additional_by_target(
    available_props_by_target: Mapping[str, Mapping[str, str]],
    known_props_by_target: Mapping[str, set[str]],
) -> dict[str, tuple[tuple[str, str], ...]]:
    additional: dict[str, tuple[tuple[str, str], ...]] = {}
    for target_path in _TARGET_PATHS:
        available = available_props_by_target.get(target_path, {})
        known_props = known_props_by_target.get(target_path, set())
        extra_names = sorted(
            prop_name
            for prop_name in available.keys()
            if prop_name not in known_props
        )
        if extra_names:
            additional[target_path] = tuple(
                (prop_name, available[prop_name])
                for prop_name in extra_names
            )
    return additional


def _append_additional_sections(
    specs: tuple[tuple[str, str, tuple[tuple[str, str], ...]], ...],
    additional_by_target: Mapping[str, tuple[tuple[str, str], ...]],
    engine_key: str,
) -> tuple[tuple[str, str, tuple[tuple[str, str], ...]], ...]:
    merged = list(specs)
    for target_path in _ENGINE_TARGET_ORDER[engine_key]:
        extra_props = additional_by_target.get(target_path)
        if not extra_props:
            continue
        merged.append((_ADDITIONAL_TITLES[target_path], target_path, extra_props))
    return tuple(merged)


def _serialize_specs(
    specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
) -> list[dict[str, Any]]:
    return [
        {
            "title": section_title,
            "target_path": target_path,
            "props": [[prop_name, label] for prop_name, label in props],
        }
        for section_title, target_path, props in specs
    ]


def _deserialize_specs(data: Any) -> tuple[tuple[str, str, tuple[tuple[str, str], ...]], ...]:
    if not isinstance(data, Sequence):
        return ()

    specs: list[tuple[str, str, tuple[tuple[str, str], ...]]] = []
    for entry in data:
        if not isinstance(entry, Mapping):
            continue
        section_title = str(entry.get("title", "")).strip()
        target_path = str(entry.get("target_path", "")).strip()
        raw_props = entry.get("props")
        if not section_title or not target_path or not isinstance(raw_props, Sequence):
            continue

        props: list[tuple[str, str]] = []
        for raw_prop in raw_props:
            if not isinstance(raw_prop, Sequence) or len(raw_prop) != 2:
                continue
            prop_name = str(raw_prop[0]).strip()
            label = str(raw_prop[1]).strip()
            if prop_name and label:
                props.append((prop_name, label))
        if props:
            specs.append((section_title, target_path, tuple(props)))
    return tuple(specs)


def _load_persisted_catalog(version_key: tuple[int, int, int]) -> dict[str, object] | None:
    filepath = _catalog_filepath(create=False)
    if not filepath or not os.path.isfile(filepath):
        return None

    try:
        with open(filepath, "r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except Exception:
        return None

    if not isinstance(payload, Mapping):
        return None
    if int(payload.get("catalog_format_version", 0)) != _CATALOG_FORMAT_VERSION:
        return None

    raw_version = payload.get("version_key")
    if not isinstance(raw_version, Sequence):
        return None
    try:
        stored_version = tuple(int(part) for part in raw_version[:3])
    except Exception:
        return None
    if stored_version != version_key:
        return None

    catalog = {
        "version_key": version_key,
        "eevee_specs": _deserialize_specs(payload.get("eevee_specs")),
        "cycles_specs": _deserialize_specs(payload.get("cycles_specs")),
    }
    _CATALOG_CACHE_BY_VERSION.clear()
    _CATALOG_CACHE_BY_VERSION[version_key] = catalog
    return catalog


def _write_persisted_catalog(catalog: Mapping[str, object]) -> None:
    filepath = _catalog_filepath(create=True)
    if not filepath:
        return

    payload = {
        "catalog_format_version": _CATALOG_FORMAT_VERSION,
        "version_key": list(catalog.get("version_key", ())),
        "eevee_specs": _serialize_specs(catalog.get("eevee_specs", ())),
        "cycles_specs": _serialize_specs(catalog.get("cycles_specs", ())),
    }
    temp_filepath = f"{filepath}.tmp"
    try:
        with open(temp_filepath, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
        os.replace(temp_filepath, filepath)
    except Exception:
        try:
            if os.path.exists(temp_filepath):
                os.remove(temp_filepath)
        except OSError:
            pass


def load_view_layer_pass_catalog(
    *,
    eevee_specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
    cycles_specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
    known_props_by_target: Mapping[str, Iterable[str]] | None = None,
) -> dict[str, object]:
    version_key = _version_key()
    cached = _CATALOG_CACHE_BY_VERSION.get(version_key)
    if cached is not None:
        return cached
    persisted = _load_persisted_catalog(version_key)
    if persisted is not None:
        return persisted

    owners = _collect_target_owners()
    available_props_by_target = {
        target_path: _collect_pass_like_boolean_properties(owner)
        for target_path, owner in owners.items()
    }
    known_props = _build_known_props_by_target(
        eevee_specs,
        cycles_specs,
        known_props_by_target,
    )

    filtered_eevee_specs = _filter_specs_by_availability(eevee_specs, available_props_by_target)
    filtered_cycles_specs = _filter_specs_by_availability(cycles_specs, available_props_by_target)
    additional_by_target = _build_additional_by_target(available_props_by_target, known_props)

    catalog = {
        "version_key": version_key,
        "eevee_specs": _append_additional_sections(
            filtered_eevee_specs,
            additional_by_target,
            "eevee_specs",
        ),
        "cycles_specs": _append_additional_sections(
            filtered_cycles_specs,
            additional_by_target,
            "cycles_specs",
        ),
    }

    # Keep the latest computed Blender-version catalog and drop older entries.
    _CATALOG_CACHE_BY_VERSION.clear()
    _CATALOG_CACHE_BY_VERSION[version_key] = catalog
    _write_persisted_catalog(catalog)
    return catalog


__all__ = (
    "TARGET_VIEW_LAYER",
    "TARGET_VIEW_LAYER_EEVEE",
    "TARGET_VIEW_LAYER_CYCLES",
    "load_view_layer_pass_catalog",
)
