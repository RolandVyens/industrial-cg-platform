# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Build- and specification-keyed pass property catalog for the ViewLayer manager."""

from __future__ import annotations

import hashlib
import json
import os
from typing import Any, Iterable, Mapping, Sequence

import bpy

from .blender_adapter import (
    TARGET_VIEW_LAYER,
    TARGET_VIEW_LAYER_CYCLES,
    TARGET_VIEW_LAYER_EEVEE,
    resolve_target,
)


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
_CATALOG_FORMAT_VERSION = 2
_CATALOG_SUBDIR = "cache"
_CATALOG_FILENAME = "view_layer_pass_catalog.json"

_CATALOG_CACHE: dict[str, dict[str, object]] = {}


def _version_key() -> tuple[int, int, int]:
    version = tuple(int(part) for part in bpy.app.version[:3])
    if len(version) < 3:
        version = version + (0,) * (3 - len(version))
    return (version[0], version[1], version[2])


def _build_hash() -> str:
    return str(getattr(bpy.app, "build_hash", "") or "unknown")


def _spec_fingerprint(
    eevee_specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
    cycles_specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
    known_props_by_target: Mapping[str, Iterable[str]] | None,
) -> str:
    payload = {
        "eevee_specs": _serialize_specs(eevee_specs),
        "cycles_specs": _serialize_specs(cycles_specs),
        "known_props_by_target": {
            target_path: sorted(str(prop_name) for prop_name in prop_names)
            for target_path, prop_names in sorted((known_props_by_target or {}).items())
        },
    }
    encoded = json.dumps(payload, ensure_ascii=True, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def _catalog_identity(
    eevee_specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
    cycles_specs: Sequence[tuple[str, str, Sequence[tuple[str, str]]]],
    known_props_by_target: Mapping[str, Iterable[str]] | None,
) -> dict[str, object]:
    return {
        "version_key": _version_key(),
        "build_hash": _build_hash(),
        "spec_fingerprint": _spec_fingerprint(eevee_specs, cycles_specs, known_props_by_target),
    }


def _serialize_identity(identity: Mapping[str, object]) -> dict[str, object]:
    version_key = identity.get("version_key", ())
    if not isinstance(version_key, Sequence):
        version_key = ()
    return {
        "version_key": [int(part) for part in version_key[:3]],
        "build_hash": str(identity.get("build_hash", "")),
        "spec_fingerprint": str(identity.get("spec_fingerprint", "")),
    }


def _identity_cache_key(identity: Mapping[str, object]) -> str:
    return json.dumps(_serialize_identity(identity), sort_keys=True, separators=(",", ":"))


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


def _fallback_target_owner(target_path: str):
    for type_name in _FALLBACK_TYPE_NAMES.get(target_path, ()):
        owner = getattr(bpy.types, type_name, None)
        if owner is not None:
            return owner
    return None


def _collect_target_owners() -> dict[str, object | None]:
    view_layer = _sample_view_layer()
    owners: dict[str, object | None] = {
        target_path: resolve_target(view_layer, target_path)
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


def _catalog_is_available(
    catalog: Mapping[str, object],
    available_props_by_target: Mapping[str, Mapping[str, str]],
) -> bool:
    for specs_key in ("eevee_specs", "cycles_specs"):
        for _title, target_path, props in catalog.get(specs_key, ()):
            available_props = available_props_by_target.get(target_path, {})
            if any(prop_name not in available_props for prop_name, _label in props):
                return False
    return True


def _load_persisted_catalog(
    identity: Mapping[str, object],
    available_props_by_target: Mapping[str, Mapping[str, str]],
) -> dict[str, object] | None:
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

    if payload.get("identity") != _serialize_identity(identity):
        return None

    catalog = {
        "version_key": tuple(identity["version_key"]),
        "identity": dict(identity),
        "eevee_specs": _deserialize_specs(payload.get("eevee_specs")),
        "cycles_specs": _deserialize_specs(payload.get("cycles_specs")),
    }
    if not _catalog_is_available(catalog, available_props_by_target):
        return None
    _CATALOG_CACHE.clear()
    _CATALOG_CACHE[_identity_cache_key(identity)] = catalog
    return catalog


def _write_persisted_catalog(catalog: Mapping[str, object]) -> None:
    filepath = _catalog_filepath(create=True)
    if not filepath:
        return

    payload = {
        "catalog_format_version": _CATALOG_FORMAT_VERSION,
        "identity": _serialize_identity(catalog.get("identity", {})),
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
    identity = _catalog_identity(eevee_specs, cycles_specs, known_props_by_target)
    identity_key = _identity_cache_key(identity)
    owners = _collect_target_owners()
    available_props_by_target = {
        target_path: _collect_pass_like_boolean_properties(owner)
        for target_path, owner in owners.items()
    }

    cached = _CATALOG_CACHE.get(identity_key)
    if cached is not None:
        if _catalog_is_available(cached, available_props_by_target):
            return cached
        _CATALOG_CACHE.pop(identity_key, None)
    persisted = _load_persisted_catalog(identity, available_props_by_target)
    if persisted is not None:
        return persisted

    known_props = _build_known_props_by_target(
        eevee_specs,
        cycles_specs,
        known_props_by_target,
    )

    filtered_eevee_specs = _filter_specs_by_availability(eevee_specs, available_props_by_target)
    filtered_cycles_specs = _filter_specs_by_availability(cycles_specs, available_props_by_target)
    additional_by_target = _build_additional_by_target(available_props_by_target, known_props)

    catalog = {
        "version_key": tuple(identity["version_key"]),
        "identity": identity,
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

    _CATALOG_CACHE.clear()
    _CATALOG_CACHE[identity_key] = catalog
    _write_persisted_catalog(catalog)
    return catalog


__all__ = (
    "TARGET_VIEW_LAYER",
    "TARGET_VIEW_LAYER_EEVEE",
    "TARGET_VIEW_LAYER_CYCLES",
    "load_view_layer_pass_catalog",
)
