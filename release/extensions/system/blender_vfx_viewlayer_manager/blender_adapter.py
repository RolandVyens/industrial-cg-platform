# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Blender data access and mutation boundary for the ViewLayer Manager."""

from __future__ import annotations


TARGET_VIEW_LAYER = "view_layer"
TARGET_VIEW_LAYER_EEVEE = "view_layer.eevee"
TARGET_VIEW_LAYER_CYCLES = "view_layer.cycles"


def resolve_target(view_layer, target_path: str):
    if target_path == TARGET_VIEW_LAYER:
        return view_layer
    if target_path == TARGET_VIEW_LAYER_EEVEE:
        return getattr(view_layer, "eevee", None) if view_layer is not None else None
    if target_path == TARGET_VIEW_LAYER_CYCLES:
        return getattr(view_layer, "cycles", None) if view_layer is not None else None
    raise KeyError(target_path)


def set_if_changed(owner, prop_name: str, value) -> bool:
    if owner is None or getattr(owner, prop_name) == value:
        return False
    setattr(owner, prop_name, value)
    return True


class ViewLayerAdapter:
    """Own ViewLayer lookup and mutation independently of the Qt window."""

    def __init__(self, scene):
        self.scene = scene

    def layers(self) -> list:
        return list(self.scene.view_layers)

    def names(self) -> list[str]:
        return [view_layer.name for view_layer in self.scene.view_layers]

    def find(self, view_layer_name: str):
        for view_layer in self.scene.view_layers:
            if view_layer.name == view_layer_name:
                return view_layer
        return None

    def selected(self, primary_name: str, active_name: str):
        view_layer = self.find(primary_name)
        if view_layer is not None:
            return view_layer
        view_layer = self.find(active_name)
        if view_layer is not None:
            return view_layer
        layers = self.layers()
        return layers[0] if layers else None

    def resolve_names(self, view_layer_names) -> list:
        return [
            view_layer
            for view_layer_name in view_layer_names
            if (view_layer := self.find(view_layer_name)) is not None
        ]

    def set_property(
        self,
        view_layer_name: str,
        target_path: str,
        prop_name: str,
        value,
    ) -> bool:
        view_layer = self.find(view_layer_name)
        owner = resolve_target(view_layer, target_path)
        return set_if_changed(owner, prop_name, value)

    def reorder(self, desired_names: list[str]) -> bool:
        current_names = self.names()
        if current_names == desired_names:
            return False
        if len(current_names) != len(desired_names) or set(current_names) != set(desired_names):
            raise ValueError("ViewLayer reorder must contain every current layer exactly once")

        for target_index, view_layer_name in enumerate(desired_names):
            current_index = current_names.index(view_layer_name)
            if current_index == target_index:
                continue
            self.scene.view_layers.move(current_index, target_index)
            moved_name = current_names.pop(current_index)
            current_names.insert(target_index, moved_name)
        return True
