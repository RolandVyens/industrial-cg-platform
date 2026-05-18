# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Bundled Blender VFX ViewLayer manager extension entrypoint."""

from __future__ import annotations

from .i18n import (
    add_translation_entry,
    pgettext_iface,
    pgettext_rpt,
    pgettext_tip,
    register_translations,
    unregister_translations,
)
from .manager import show_manager
from .presets import (
    apply_named_pass_preset,
    apply_pass_preset,
    collect_pass_preset,
    delete_pass_preset,
    get_preset_directory,
    get_preset_filepath,
    list_pass_presets,
    load_pass_preset,
    save_pass_preset,
)


def register() -> None:
    # Registration stays intentionally light so the extension can be enabled
    # before the bundled Qt runtime has been staged into the release payload.
    register_translations()
    return None


def unregister() -> None:
    unregister_translations()
    return None


__all__ = (
    "show_manager",
    "add_translation_entry",
    "pgettext_iface",
    "pgettext_tip",
    "pgettext_rpt",
    "register_translations",
    "unregister_translations",
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
