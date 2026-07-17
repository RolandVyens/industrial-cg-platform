# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Qt window implementation for the bundled ViewLayer manager."""

from __future__ import annotations

import bpy
from bqt.utils import context_window
from PySide6 import QtCore, QtGui, QtWidgets

from .blender_adapter import (
    TARGET_VIEW_LAYER,
    TARGET_VIEW_LAYER_CYCLES,
    ViewLayerAdapter,
    resolve_target,
    set_if_changed,
)
from .i18n import pgettext_iface as iface_, pgettext_tip as tip_
from .property_catalog import load_view_layer_pass_catalog
from .presets import (
    apply_live_pass_state,
    apply_pass_preset_to_view_layers,
    delete_pass_preset,
    load_pass_preset,
    list_pass_presets,
    save_pass_preset,
)


WINDOW_OBJECT_NAME = "blender_vfx_viewlayer_manager_window"
ENGINE_BLENDER_EEVEE = "BLENDER_EEVEE"
ENGINE_CYCLES = "CYCLES"
CLASSIC_DETAIL_LIST_HEIGHT = 156
SMOOTH_SCROLL_SINGLE_STEP = 18
SMOOTH_SCROLL_PAGE_STEP = 72
COLOR_TEXT_DEFAULT = "#cfd8e3"
COLOR_TEXT_STRONG = "#f3f7fb"
COLOR_TEXT_MUTED = "#98a8bb"
COLOR_SELECTION_FILL = "#35506f"
COLOR_SELECTION_FILL_STRONG = "#3f5f85"
COLOR_SELECTION_BORDER = "#7fb1e8"
COLOR_ACTIVE_BORDER = "#d79b4f"
COLOR_SURFACE = "#20262e"
COLOR_SURFACE_ELEVATED = "#252d38"
COLOR_SURFACE_CARD = "#1a1f26"

EEVEE_PASS_SPECS = (
    ("Data", "view_layer", (
        ("use_pass_combined", "Combined"),
        ("use_pass_z", "Z"),
        ("use_pass_mist", "Mist"),
        ("use_pass_normal", "Normal"),
        ("use_pass_position", "Position"),
        ("use_pass_vector", "Vector"),
        ("use_pass_grease_pencil", "Grease Pencil"),
    )),
    ("Light", "view_layer", (
        ("use_pass_diffuse_direct", "Diffuse Light"),
        ("use_pass_glossy_direct", "Specular Light"),
        ("use_pass_emit", "Emission"),
        ("use_pass_environment", "Environment"),
        ("use_pass_shadow", "Shadow"),
        ("use_pass_ambient_occlusion", "Ambient Occlusion"),
    )),
    ("Shader", "view_layer", (
        ("use_pass_diffuse_color", "Diffuse Color"),
        ("use_pass_glossy_color", "Specular Color"),
    )),
    ("Effects / Utility", "view_layer.eevee", (
        ("use_pass_volume_direct", "Volume Light"),
        ("use_pass_transparent", "Transparent"),
    )),
)

CYCLES_PASS_SPECS = (
    ("Data", "view_layer", (
        ("use_pass_combined", "Combined"),
        ("use_pass_z", "Z"),
        ("use_pass_mist", "Mist"),
        ("use_pass_position", "Position"),
        ("use_pass_normal", "Normal"),
        ("use_pass_vector", "Vector"),
        ("use_pass_uv", "UV"),
        ("use_pass_grease_pencil", "Grease Pencil"),
        ("use_pass_object_index", "Object Index"),
        ("use_pass_material_index", "Material Index"),
    )),
    ("Light", "view_layer", (
        ("use_pass_diffuse_direct", "Diffuse Direct"),
        ("use_pass_diffuse_indirect", "Diffuse Indirect"),
        ("use_pass_glossy_direct", "Glossy Direct"),
        ("use_pass_glossy_indirect", "Glossy Indirect"),
        ("use_pass_transmission_direct", "Transmission Direct"),
        ("use_pass_transmission_indirect", "Transmission Indirect"),
        ("use_pass_emit", "Emission"),
        ("use_pass_environment", "Environment"),
        ("use_pass_ambient_occlusion", "Ambient Occlusion"),
    )),
    ("Shader", "view_layer", (
        ("use_pass_diffuse_color", "Diffuse Color"),
        ("use_pass_glossy_color", "Glossy Color"),
        ("use_pass_transmission_color", "Transmission Color"),
    )),
    ("Effects / Utility", "view_layer.cycles", (
        ("use_pass_volume_direct", "Volume Direct"),
        ("use_pass_volume_indirect", "Volume Indirect"),
        ("use_pass_shadow_catcher", "Shadow Catcher"),
    )),
)

CYCLES_LIGHT_PASS_AOV_SPECS = (
    ("diffuse", "Diffuse"),
    ("glossy", "Glossy"),
    ("transmission", "Transmission"),
    ("volume", "Volume"),
)

CRYPTOMATTE_BOOLEAN_SPECS = (
    ("use_pass_cryptomatte_object", "Object"),
    ("use_pass_cryptomatte_material", "Material"),
    ("use_pass_cryptomatte_asset", "Asset"),
)
CRYPTOMATTE_LEVELS_PROP = "pass_cryptomatte_depth"


def _configure_smooth_scroll(
    view: QtWidgets.QAbstractScrollArea,
    *,
    horizontal: bool = False,
) -> None:
    if isinstance(view, QtWidgets.QAbstractItemView):
        view.setVerticalScrollMode(QtWidgets.QAbstractItemView.ScrollMode.ScrollPerPixel)
        if horizontal:
            view.setHorizontalScrollMode(QtWidgets.QAbstractItemView.ScrollMode.ScrollPerPixel)
    vertical_scrollbar = view.verticalScrollBar()
    vertical_scrollbar.setSingleStep(SMOOTH_SCROLL_SINGLE_STEP)
    vertical_scrollbar.setPageStep(SMOOTH_SCROLL_PAGE_STEP)
    if horizontal:
        horizontal_scrollbar = view.horizontalScrollBar()
        horizontal_scrollbar.setSingleStep(SMOOTH_HORIZONTAL_SCROLL_SINGLE_STEP)
        horizontal_scrollbar.setPageStep(CARD_WIDTH)


def _notify_active_view_layer_changed(window: "ViewLayerManagerWindow") -> None:
    window._queue_active_view_layer_sync()


def _build_toolbar(*buttons: QtWidgets.QPushButton) -> QtWidgets.QHBoxLayout:
    layout = QtWidgets.QHBoxLayout()
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(6)
    for button in buttons:
        layout.addWidget(button)
    layout.addStretch(1)
    return layout


def _apply_list_selection_style(widget: QtWidgets.QListWidget) -> None:
    widget.setStyleSheet(
        "QListWidget {"
        " background: transparent;"
        f" color: {COLOR_TEXT_DEFAULT};"
        " border: none;"
        " outline: 0;"
        "}"
        "QListWidget::item {"
        " padding: 4px 8px;"
        " border-radius: 6px;"
        f" color: {COLOR_TEXT_DEFAULT};"
        "}"
        "QListWidget::item:hover {"
        f" background-color: {COLOR_SURFACE_ELEVATED};"
        f" color: {COLOR_TEXT_STRONG};"
        "}"
        "QListWidget::item:selected {"
        f" background-color: {COLOR_SELECTION_FILL_STRONG};"
        f" color: {COLOR_TEXT_STRONG};"
        f" border: 1px solid {COLOR_SELECTION_BORDER};"
        "}"
    )


def _update_list_item_foregrounds(widget: QtWidgets.QListWidget) -> None:
    current_row = widget.currentRow()
    for row in range(widget.count()):
        item = widget.item(row)
        if item is None:
            continue
        color = COLOR_TEXT_STRONG if row == current_row else COLOR_TEXT_DEFAULT
        item.setForeground(QtGui.QColor(color))


def _configure_detail_list(widget: QtWidgets.QListWidget, *, height: int) -> None:
    widget.setMinimumHeight(height)
    widget.setMaximumHeight(height)
    widget.setVerticalScrollMode(QtWidgets.QAbstractItemView.ScrollMode.ScrollPerPixel)
    widget.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    widget.setVerticalScrollBarPolicy(QtCore.Qt.ScrollBarPolicy.ScrollBarAsNeeded)
    _apply_list_selection_style(widget)
    _configure_smooth_scroll(widget)


def _tr(text: str) -> str:
    return iface_(text)


class BrushCheckBox(QtWidgets.QCheckBox):
    def __init__(self, manager: "ViewLayerManagerWindow", text: str = ""):
        super().__init__(text)
        self._manager = manager
        self._brush_meta: dict[str, object] = {}

    @property
    def brush_meta(self) -> dict[str, object]:
        return self._brush_meta

    def set_brush_meta(self, metadata: dict[str, object]) -> None:
        self._brush_meta = metadata

    def mousePressEvent(self, event) -> None:
        if event.button() == QtCore.Qt.MouseButton.LeftButton and self.isEnabled():
            if self._manager._begin_checkbox_brush(self):
                event.accept()
                return
        super().mousePressEvent(event)


class NoWheelSpinBox(QtWidgets.QSpinBox):
    def wheelEvent(self, event) -> None:
        event.ignore()


class ViewLayerListRowWidget(QtWidgets.QFrame):
    clicked = QtCore.Signal(str, int)

    def __init__(self, manager: "ViewLayerManagerWindow", view_layer_name: str):
        super().__init__()
        self._manager = manager
        self._view_layer_name = view_layer_name
        self._is_active = False
        self._is_selected = False

        self.setObjectName("ViewLayerListRowWidget")
        self.setFrameShape(QtWidgets.QFrame.Shape.StyledPanel)
        self.setCursor(QtCore.Qt.CursorShape.PointingHandCursor)

        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(8, 4, 8, 4)
        layout.setSpacing(8)

        self.name_label = QtWidgets.QLabel(view_layer_name)
        self.name_label.setAttribute(QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        layout.addWidget(self.name_label, 1)

        self.use_checkbox = self._manager._new_brush_checkbox(
            _tr("Use"),
            kind="view_layer_use",
            view_layer_name_getter=lambda: self.view_layer_name,
        )
        self.use_checkbox.setToolTip(tip_("Use For Rendering"))
        layout.addWidget(self.use_checkbox)
        self._update_style()

    @property
    def view_layer_name(self) -> str:
        return self._view_layer_name

    def sync_from_view_layer(self, view_layer, *, is_active: bool, is_selected: bool) -> None:
        self._view_layer_name = view_layer.name
        self._is_active = is_active
        self._is_selected = is_selected

        self.name_label.setText(view_layer.name)
        font = self.name_label.font()
        font.setBold(is_active)
        self.name_label.setFont(font)

        self.use_checkbox.blockSignals(True)
        self.use_checkbox.setChecked(view_layer.use)
        self.use_checkbox.blockSignals(False)
        self._update_style()

    def mousePressEvent(self, event) -> None:
        if event.button() == QtCore.Qt.MouseButton.LeftButton:
            modifiers = event.modifiers()
            modifier_value = getattr(modifiers, "value", modifiers)
            self.clicked.emit(self._view_layer_name, int(modifier_value))
            event.accept()
            return
        super().mousePressEvent(event)

    def _update_style(self) -> None:
        background = COLOR_SELECTION_FILL_STRONG if self._is_selected else COLOR_SURFACE
        border = COLOR_ACTIVE_BORDER if self._is_active else (COLOR_SELECTION_BORDER if self._is_selected else "#37414d")
        text_color = COLOR_TEXT_STRONG if (self._is_selected or self._is_active) else COLOR_TEXT_DEFAULT
        self.setStyleSheet(
            "QFrame#ViewLayerListRowWidget {"
            f"background-color: {background};"
            f"border: 1px solid {border};"
            "border-radius: 6px;"
            "}"
        )
        self.name_label.setStyleSheet(f"color: {text_color};")
        self.use_checkbox.setStyleSheet(f"color: {text_color};")


class ViewLayerManagerWindow(QtWidgets.QDialog):
    def __init__(self):
        super().__init__()

        self._active_state_timer = QtCore.QTimer(self)
        self._active_state_timer.setInterval(150)
        self._active_state_timer.timeout.connect(self._poll_active_view_layer_state)
        self._last_active_view_layer_name = ""
        self._msgbus_owner = object()
        self._msgbus_registered = False
        self._selected_view_layer_name = ""
        self._selected_view_layer_names: list[str] = []
        self._classic_selection_anchor_name = ""
        self._selected_aov_index = -1
        self._selected_lightgroup_index = -1
        self._classic_pass_bindings: list[tuple[QtWidgets.QCheckBox, str, str]] = []
        self._classic_cycles_light_pass_bindings: list[tuple[QtWidgets.QCheckBox, str]] = []
        self._classic_cryptomatte_bindings: list[tuple[QtWidgets.QCheckBox, str]] = []
        self._classic_row_widgets: dict[str, ViewLayerListRowWidget] = {}
        self._checkbox_brush_active = False
        self._checkbox_brush_target_state = False
        self._checkbox_brush_changed = False
        self._checkbox_brush_visited: set[int] = set()
        self._checkbox_brush_source: BrushCheckBox | None = None
        self._checkbox_brush_preserved_view_layer_name = ""
        self._checkbox_brush_event_filter_installed = False
        self._refresh_from_blender_queued = False
        self._preset_combo_refreshing = False
        self._pass_catalog: dict[str, object] = self._load_pass_catalog()

        self.setObjectName(WINDOW_OBJECT_NAME)
        self.setWindowTitle(_tr("ViewLayer Manager"))
        self.resize(1220, 820)

        self._build_ui()
        self._connect_signals()
        self._register_message_bus()
        self._reload_pass_preset_combo()
        self.refresh_from_blender()

    def _build_ui(self) -> None:
        root = QtWidgets.QVBoxLayout(self)

        header_layout = QtWidgets.QHBoxLayout()
        self.scene_label = QtWidgets.QLabel(f"{_tr('Scene')}: -")
        self.active_view_layer_label = QtWidgets.QLabel(f"{_tr('Active ViewLayer')}: -")
        self.preset_label = QtWidgets.QLabel(_tr("Pass Preset"))
        self.preset_combo = QtWidgets.QComboBox()
        self.preset_combo.setMinimumWidth(240)
        self.save_new_preset_button = QtWidgets.QPushButton(_tr("Save New"))
        self.update_preset_button = QtWidgets.QPushButton(_tr("Update"))
        self.apply_preset_button = QtWidgets.QPushButton(_tr("Apply"))
        self.delete_preset_button = QtWidgets.QPushButton(_tr("Delete"))
        self.engine_label = QtWidgets.QLabel(f"{_tr('Engine')}: -")
        header_layout.addWidget(self.scene_label)
        header_layout.addWidget(self.active_view_layer_label)
        header_layout.addSpacing(10)
        header_layout.addWidget(self.preset_label)
        header_layout.addWidget(self.preset_combo)
        header_layout.addWidget(self.save_new_preset_button)
        header_layout.addWidget(self.update_preset_button)
        header_layout.addWidget(self.apply_preset_button)
        header_layout.addWidget(self.delete_preset_button)
        header_layout.addStretch(1)
        header_layout.addWidget(self.engine_label)
        root.addLayout(header_layout)

        self.mode_stack = QtWidgets.QStackedWidget()
        root.addWidget(self.mode_stack, 1)

        self._build_classic_page()

        footer = QtWidgets.QHBoxLayout()
        footer.addStretch(1)
        self.close_button = QtWidgets.QPushButton(_tr("Close"))
        footer.addWidget(self.close_button)
        root.addLayout(footer)

    def _build_classic_page(self) -> None:
        self.classic_page = QtWidgets.QWidget()
        classic_root = QtWidgets.QHBoxLayout(self.classic_page)

        splitter = QtWidgets.QSplitter(QtCore.Qt.Orientation.Horizontal)
        classic_root.addWidget(splitter, 1)

        left_panel = QtWidgets.QWidget()
        left_layout = QtWidgets.QVBoxLayout(left_panel)
        left_layout.addWidget(QtWidgets.QLabel(_tr("View Layers")))
        self.view_layer_list = QtWidgets.QListWidget()
        self.view_layer_list.setFrameShape(QtWidgets.QFrame.Shape.NoFrame)
        self.view_layer_list.setSpacing(4)
        self.view_layer_list.setSelectionMode(QtWidgets.QAbstractItemView.SelectionMode.ExtendedSelection)
        self.view_layer_list.setStyleSheet("QListWidget { background: transparent; border: none; outline: 0; }")
        _configure_smooth_scroll(self.view_layer_list)
        left_layout.addWidget(self.view_layer_list, 1)

        classic_button_grid = QtWidgets.QGridLayout()
        self.add_view_layer_button = QtWidgets.QPushButton(_tr("Add"))
        self.delete_view_layer_button = QtWidgets.QPushButton(_tr("Delete"))
        self.move_view_layer_up_button = QtWidgets.QPushButton(_tr("Up"))
        self.move_view_layer_down_button = QtWidgets.QPushButton(_tr("Down"))
        classic_button_grid.addWidget(self.add_view_layer_button, 0, 0)
        classic_button_grid.addWidget(self.delete_view_layer_button, 0, 1)
        classic_button_grid.addWidget(self.move_view_layer_up_button, 1, 0)
        classic_button_grid.addWidget(self.move_view_layer_down_button, 1, 1)
        left_layout.addLayout(classic_button_grid)
        splitter.addWidget(left_panel)

        right_panel = QtWidgets.QScrollArea()
        right_panel.setWidgetResizable(True)
        self.classic_right_scroll_area = right_panel
        right_panel_widget = QtWidgets.QWidget()
        self.classic_right_layout = QtWidgets.QVBoxLayout(right_panel_widget)
        right_panel.setWidget(right_panel_widget)
        splitter.addWidget(right_panel)
        splitter.setStretchFactor(1, 1)

        self.basic_group = QtWidgets.QGroupBox(_tr("Basic"))
        basic_form = QtWidgets.QFormLayout(self.basic_group)
        self.view_layer_name_edit = QtWidgets.QLineEdit()
        self.view_layer_use_checkbox = self._new_brush_checkbox(
            _tr("Use For Rendering"),
            kind="view_layer_use",
            view_layer_name_getter=self._selected_view_layer_name_for_checkbox,
        )
        self.view_layer_deep_checkbox = self._new_brush_checkbox(
            _tr("Deep"),
            kind="view_layer_deep",
            view_layer_name_getter=self._selected_view_layer_name_for_checkbox,
        )
        self.view_layer_samples_spin = NoWheelSpinBox()
        self.view_layer_samples_spin.setRange(0, 1000000)
        basic_form.addRow(_tr("Name"), self.view_layer_name_edit)
        basic_form.addRow("", self.view_layer_use_checkbox)
        basic_form.addRow("", self.view_layer_deep_checkbox)
        basic_form.addRow(_tr("Samples"), self.view_layer_samples_spin)
        self.classic_right_layout.addWidget(self.basic_group)

        eevee_specs = self._pass_catalog.get("eevee_specs", EEVEE_PASS_SPECS)
        cycles_specs = self._pass_catalog.get("cycles_specs", CYCLES_PASS_SPECS)
        self.eevee_passes_group = self._build_classic_pass_group(_tr("Eevee Passes"), eevee_specs)
        self.cycles_passes_group = self._build_classic_pass_group(_tr("Cycles Passes"), cycles_specs)
        self.classic_right_layout.addWidget(self.eevee_passes_group)
        self.classic_right_layout.addWidget(self.cycles_passes_group)

        self.cryptomatte_group = QtWidgets.QGroupBox(_tr("Cryptomatte"))
        cryptomatte_layout = QtWidgets.QVBoxLayout(self.cryptomatte_group)
        for prop_name, label in CRYPTOMATTE_BOOLEAN_SPECS:
            checkbox = self._new_brush_checkbox(
                _tr(label),
                kind="view_layer_pass_property",
                view_layer_name_getter=self._selected_view_layer_name_for_checkbox,
                target_path="view_layer",
                prop_name=prop_name,
            )
            cryptomatte_layout.addWidget(checkbox)
            self._classic_pass_bindings.append((checkbox, "view_layer", prop_name))
            self._classic_cryptomatte_bindings.append((checkbox, prop_name))
        cryptomatte_form = QtWidgets.QFormLayout()
        self.cryptomatte_levels_spin = NoWheelSpinBox()
        self.cryptomatte_levels_spin.setRange(2, 16)
        self.cryptomatte_levels_spin.setSingleStep(2)
        cryptomatte_form.addRow(_tr("Levels"), self.cryptomatte_levels_spin)
        cryptomatte_layout.addLayout(cryptomatte_form)
        self.classic_right_layout.addWidget(self.cryptomatte_group)

        self.aov_group = QtWidgets.QGroupBox(_tr("Shader AOV"))
        aov_layout = QtWidgets.QVBoxLayout(self.aov_group)
        self.add_aov_button = QtWidgets.QPushButton(_tr("Add"))
        self.remove_aov_button = QtWidgets.QPushButton(_tr("Remove"))
        aov_layout.addLayout(_build_toolbar(self.add_aov_button, self.remove_aov_button))
        self.aov_list = QtWidgets.QListWidget()
        _configure_detail_list(self.aov_list, height=CLASSIC_DETAIL_LIST_HEIGHT)
        aov_layout.addWidget(self.aov_list)

        aov_form = QtWidgets.QFormLayout()
        self.aov_name_edit = QtWidgets.QLineEdit()
        self.aov_type_combo = QtWidgets.QComboBox()
        self.aov_type_combo.addItems(["VALUE", "COLOR"])
        aov_form.addRow(_tr("Name"), self.aov_name_edit)
        aov_form.addRow(_tr("Type"), self.aov_type_combo)
        aov_layout.addLayout(aov_form)
        self.aov_warning_label = QtWidgets.QLabel("")
        self.aov_warning_label.setStyleSheet("color: #d36b6b;")
        aov_layout.addWidget(self.aov_warning_label)
        self.classic_right_layout.addWidget(self.aov_group)

        self.lightgroup_group = QtWidgets.QGroupBox(_tr("Light Groups"))
        lightgroup_layout = QtWidgets.QVBoxLayout(self.lightgroup_group)
        self.add_lightgroup_button = QtWidgets.QPushButton(_tr("Add"))
        self.remove_lightgroup_button = QtWidgets.QPushButton(_tr("Remove"))
        self.add_used_lightgroups_button = QtWidgets.QPushButton(_tr("Add Used"))
        self.remove_unused_lightgroups_button = QtWidgets.QPushButton(_tr("Remove Unused"))
        lightgroup_layout.addLayout(
            _build_toolbar(
                self.add_lightgroup_button,
                self.remove_lightgroup_button,
                self.add_used_lightgroups_button,
                self.remove_unused_lightgroups_button,
            )
        )
        self.lightgroup_list = QtWidgets.QListWidget()
        _configure_detail_list(self.lightgroup_list, height=CLASSIC_DETAIL_LIST_HEIGHT)
        lightgroup_layout.addWidget(self.lightgroup_list)

        lightgroup_form = QtWidgets.QFormLayout()
        self.lightgroup_name_edit = QtWidgets.QLineEdit()
        lightgroup_form.addRow(_tr("Name"), self.lightgroup_name_edit)
        lightgroup_layout.addLayout(lightgroup_form)
        self.classic_right_layout.addWidget(self.lightgroup_group)

        self.cycles_light_pass_group = QtWidgets.QGroupBox(_tr("Cycles Light Pass AOVs"))
        cycles_layout = QtWidgets.QVBoxLayout(self.cycles_light_pass_group)
        self.use_lightgroup_light_pass_aovs_checkbox = self._new_brush_checkbox(
            _tr("Enable Light Pass AOVs"),
            kind="cycles_light_pass_master",
            view_layer_name_getter=self._selected_view_layer_name_for_checkbox,
        )
        cycles_layout.addWidget(self.use_lightgroup_light_pass_aovs_checkbox)
        self.cycles_light_pass_info_label = QtWidgets.QLabel(
            _tr("Memory scales with light groups x enabled light pass AOVs")
        )
        cycles_layout.addWidget(self.cycles_light_pass_info_label)
        self.cycles_light_pass_empty_label = QtWidgets.QLabel(
            _tr("Add light groups to enable light pass AOV outputs")
        )
        cycles_layout.addWidget(self.cycles_light_pass_empty_label)

        cycles_grid = QtWidgets.QGridLayout()
        cycles_grid.addWidget(QtWidgets.QLabel(_tr("Lobe")), 0, 0)
        cycles_grid.addWidget(QtWidgets.QLabel(_tr("All")), 0, 1)
        cycles_grid.addWidget(QtWidgets.QLabel(_tr("Combined")), 0, 2)
        cycles_grid.addWidget(QtWidgets.QLabel(_tr("Direct")), 0, 3)
        cycles_grid.addWidget(QtWidgets.QLabel(_tr("Indirect")), 0, 4)
        for row_index, (lobe_id, label) in enumerate(CYCLES_LIGHT_PASS_AOV_SPECS, start=1):
            cycles_grid.addWidget(QtWidgets.QLabel(_tr(label)), row_index, 0)
            for column_index, suffix in enumerate(("all", "combined", "direct", "indirect"), start=1):
                checkbox = self._new_brush_checkbox(
                    "",
                    kind="cycles_light_pass_property",
                    view_layer_name_getter=self._selected_view_layer_name_for_checkbox,
                    prop_name=f"use_lightgroup_light_pass_aov_{lobe_id}_{suffix}",
                )
                cycles_grid.addWidget(checkbox, row_index, column_index)
                self._classic_cycles_light_pass_bindings.append((checkbox, f"use_lightgroup_light_pass_aov_{lobe_id}_{suffix}"))
        cycles_layout.addLayout(cycles_grid)
        self.classic_right_layout.addWidget(self.cycles_light_pass_group)
        self.classic_right_layout.addStretch(1)
        _configure_smooth_scroll(self.classic_right_scroll_area)

        self.mode_stack.addWidget(self.classic_page)

    def _build_classic_pass_group(self, title: str, specs):
        group = QtWidgets.QGroupBox(title)
        outer_layout = QtWidgets.QVBoxLayout(group)
        for section_title, target_path, props in specs:
            section_group = QtWidgets.QGroupBox(_tr(section_title))
            section_layout = QtWidgets.QVBoxLayout(section_group)
            for index, (prop_name, label) in enumerate(props):
                checkbox = self._new_brush_checkbox(
                    _tr(label),
                    kind="view_layer_pass_property",
                    view_layer_name_getter=self._selected_view_layer_name_for_checkbox,
                    target_path=target_path,
                    prop_name=prop_name,
                )
                section_layout.addWidget(checkbox)
                self._classic_pass_bindings.append((checkbox, target_path, prop_name))
            outer_layout.addWidget(section_group)
        return group

    def _load_pass_catalog(self) -> dict[str, object]:
        try:
            return load_view_layer_pass_catalog(
                eevee_specs=EEVEE_PASS_SPECS,
                cycles_specs=CYCLES_PASS_SPECS,
                known_props_by_target={
                    TARGET_VIEW_LAYER: {prop_name for prop_name, _label in CRYPTOMATTE_BOOLEAN_SPECS},
                },
            )
        except Exception:
            return {
                "version_key": tuple(int(part) for part in bpy.app.version[:3]),
                "eevee_specs": EEVEE_PASS_SPECS,
                "cycles_specs": CYCLES_PASS_SPECS,
            }

    def _connect_signals(self) -> None:
        self.close_button.clicked.connect(self.close)
        self.preset_combo.currentIndexChanged.connect(self._update_preset_buttons)
        self.save_new_preset_button.clicked.connect(self._save_new_pass_preset)
        self.update_preset_button.clicked.connect(self._update_selected_pass_preset)
        self.apply_preset_button.clicked.connect(self._apply_selected_pass_preset)
        self.delete_preset_button.clicked.connect(self._delete_selected_pass_preset)

        self.view_layer_name_edit.editingFinished.connect(self._apply_live_update)
        self.view_layer_samples_spin.valueChanged.connect(self._apply_live_update)
        self.cryptomatte_levels_spin.valueChanged.connect(self._apply_live_update)

        self.view_layer_list.currentRowChanged.connect(self._on_view_layer_selection_changed)
        self.view_layer_list.itemSelectionChanged.connect(self._on_view_layer_multi_selection_changed)
        self.add_view_layer_button.clicked.connect(self._add_view_layer)
        self.delete_view_layer_button.clicked.connect(self._delete_selected_view_layer)
        self.move_view_layer_up_button.clicked.connect(lambda: self._move_selected_view_layer(-1))
        self.move_view_layer_down_button.clicked.connect(lambda: self._move_selected_view_layer(1))

        self.aov_list.currentRowChanged.connect(self._on_aov_selection_changed)
        self.add_aov_button.clicked.connect(self._add_aov)
        self.remove_aov_button.clicked.connect(self._remove_aov)
        self.aov_name_edit.editingFinished.connect(self._apply_live_update)
        self.aov_type_combo.activated.connect(self._apply_live_update)

        self.lightgroup_list.currentRowChanged.connect(self._on_lightgroup_selection_changed)
        self.add_lightgroup_button.clicked.connect(self._add_lightgroup)
        self.remove_lightgroup_button.clicked.connect(self._remove_lightgroup)
        self.add_used_lightgroups_button.clicked.connect(self._add_used_lightgroups)
        self.remove_unused_lightgroups_button.clicked.connect(self._remove_unused_lightgroups)
        self.lightgroup_name_edit.editingFinished.connect(self._apply_live_update)

    def _scene(self):
        return bpy.context.scene

    def _adapter(self) -> ViewLayerAdapter:
        return ViewLayerAdapter(self._scene())

    def _blender_window(self):
        window = getattr(bpy.context, "window", None)
        if window is not None:
            return window
        windows = getattr(bpy.context.window_manager, "windows", None)
        if windows:
            return windows[0]
        return None

    def _selected_view_layer_name_for_checkbox(self) -> str:
        view_layer = self._selected_view_layer()
        return view_layer.name if view_layer is not None else ""

    def _new_brush_checkbox(
        self,
        text: str,
        *,
        kind: str,
        view_layer_name_getter=None,
        target_path: str | None = None,
        prop_name: str | None = None,
    ) -> BrushCheckBox:
        checkbox = BrushCheckBox(self, text)
        checkbox.set_brush_meta(
            {
                "kind": kind,
                "view_layer_name_getter": view_layer_name_getter,
                "target_path": target_path,
                "prop_name": prop_name,
            }
        )
        checkbox.toggled.connect(self._on_brush_checkbox_toggled)
        return checkbox

    def _resolve_checkbox_view_layer_name(self, checkbox: BrushCheckBox) -> str:
        getter = checkbox.brush_meta.get("view_layer_name_getter")
        if callable(getter):
            return getter() or ""
        return checkbox.brush_meta.get("view_layer_name", "") or ""

    def _set_checkbox_visual_state(self, checkbox: BrushCheckBox, checked: bool) -> None:
        checkbox.blockSignals(True)
        checkbox.setChecked(checked)
        checkbox.blockSignals(False)

    def _apply_checkbox_metadata(
        self,
        checkbox: BrushCheckBox,
        checked: bool,
        *,
        push_undo: bool,
        refresh: bool,
    ) -> bool:
        meta = checkbox.brush_meta
        view_layer_name = self._resolve_checkbox_view_layer_name(checkbox)
        if not view_layer_name:
            return False

        kind = meta.get("kind")
        changed = False
        if kind == "view_layer_use":
            changed = self._set_view_layer_use_in_blender(view_layer_name, checked, push_undo=push_undo)
        elif kind == "view_layer_deep":
            changed = self._set_view_layer_deep_in_blender(view_layer_name, checked, push_undo=push_undo)
        elif kind == "view_layer_pass_property":
            changed = self._set_view_layer_pass_property_in_blender(
                view_layer_name,
                meta.get("target_path"),
                meta.get("prop_name"),
                checked,
                push_undo=push_undo,
            )
        elif kind == "cycles_light_pass_master":
            changed = self._set_cycles_light_pass_master_in_blender(
                view_layer_name,
                checked,
                push_undo=push_undo,
            )
        elif kind == "cycles_light_pass_property":
            changed = self._set_cycles_light_pass_property_in_blender(
                view_layer_name,
                meta.get("prop_name"),
                checked,
                push_undo=push_undo,
            )

        if not changed:
            return False

        if refresh:
            self.refresh_from_blender()
        else:
            self._sync_checkbox_brush_dependencies(checkbox)
        return True

    def _sync_checkbox_brush_dependencies(self, checkbox: BrushCheckBox) -> None:
        if checkbox.brush_meta.get("kind") != "cycles_light_pass_master":
            return
        view_layer_name = self._resolve_checkbox_view_layer_name(checkbox)
        view_layer = self._find_view_layer(view_layer_name)
        if view_layer is None:
            return
        self._sync_cycles_light_pass_group(view_layer)

    def _install_checkbox_brush_event_filter(self) -> None:
        app = QtWidgets.QApplication.instance()
        if app is None or self._checkbox_brush_event_filter_installed:
            return
        app.installEventFilter(self)
        self._checkbox_brush_event_filter_installed = True

    def _remove_checkbox_brush_event_filter(self) -> None:
        app = QtWidgets.QApplication.instance()
        if app is not None and self._checkbox_brush_event_filter_installed:
            app.removeEventFilter(self)
        self._checkbox_brush_event_filter_installed = False

    def _reset_checkbox_brush_state(self) -> str:
        preserved_view_layer_name = self._checkbox_brush_preserved_view_layer_name
        self._checkbox_brush_active = False
        self._checkbox_brush_changed = False
        self._checkbox_brush_target_state = False
        self._checkbox_brush_visited.clear()
        self._checkbox_brush_source = None
        self._checkbox_brush_preserved_view_layer_name = ""
        return preserved_view_layer_name

    def _cancel_checkbox_brush(self) -> None:
        try:
            self._remove_checkbox_brush_event_filter()
        finally:
            preserved_view_layer_name = self._reset_checkbox_brush_state()
            if preserved_view_layer_name:
                self._selected_view_layer_name = preserved_view_layer_name

    def _is_brushable_checkbox(self, checkbox: QtWidgets.QWidget | None) -> bool:
        if not isinstance(checkbox, BrushCheckBox):
            return False
        if checkbox._manager is not self:
            return False
        if not checkbox.isEnabled():
            return False
        return self.classic_page.isVisible() and checkbox.isVisibleTo(self)

    def _find_brush_checkbox(self, widget: QtWidgets.QWidget | None) -> BrushCheckBox | None:
        current = widget
        while current is not None:
            if self._is_brushable_checkbox(current):
                return current
            current = current.parentWidget()
        return None

    def _find_classic_row_use_checkbox(self, global_pos: QtCore.QPoint) -> BrushCheckBox | None:
        if not self.classic_page.isVisible():
            return None
        viewport = self.view_layer_list.viewport()
        local_pos = viewport.mapFromGlobal(global_pos)
        if not viewport.rect().contains(local_pos):
            return None
        item = self.view_layer_list.itemAt(local_pos)
        if item is None:
            return None
        row_widget = self.view_layer_list.itemWidget(item)
        if not isinstance(row_widget, ViewLayerListRowWidget):
            return None
        checkbox = row_widget.use_checkbox
        if not self._is_brushable_checkbox(checkbox):
            return None
        row_local_pos = row_widget.mapFromGlobal(global_pos)
        hot_rect = QtCore.QRect(checkbox.geometry())
        hot_rect.setTop(0)
        hot_rect.setBottom(row_widget.height())
        hot_rect.adjust(-12, 0, 12, 0)
        if hot_rect.contains(row_local_pos):
            return checkbox
        return None

    def _apply_checkbox_brush_to(self, checkbox: BrushCheckBox) -> None:
        if not self._checkbox_brush_active or not self._is_brushable_checkbox(checkbox):
            return
        checkbox_id = id(checkbox)
        if checkbox_id in self._checkbox_brush_visited:
            return
        self._checkbox_brush_visited.add(checkbox_id)
        self._set_checkbox_visual_state(checkbox, self._checkbox_brush_target_state)
        changed = self._apply_checkbox_metadata(
            checkbox,
            self._checkbox_brush_target_state,
            push_undo=False,
            refresh=False,
        )
        self._checkbox_brush_changed |= changed

    def _brush_checkbox_from_global_pos(self, global_pos: QtCore.QPoint) -> None:
        checkbox = self._find_brush_checkbox(QtWidgets.QApplication.widgetAt(global_pos))
        if checkbox is None:
            checkbox = self._find_classic_row_use_checkbox(global_pos)
        if checkbox is not None:
            self._apply_checkbox_brush_to(checkbox)

    def _begin_checkbox_brush(self, checkbox: BrushCheckBox) -> bool:
        if not isinstance(checkbox, BrushCheckBox):
            return False
        if checkbox._manager is not self:
            return False
        if not checkbox.isEnabled():
            return False
        if self._checkbox_brush_active:
            self._end_checkbox_brush()
        self._checkbox_brush_active = True
        self._checkbox_brush_target_state = not checkbox.isChecked()
        self._checkbox_brush_changed = False
        self._checkbox_brush_visited = set()
        self._checkbox_brush_source = checkbox
        self._checkbox_brush_preserved_view_layer_name = self._selected_view_layer_name or ""
        try:
            self._install_checkbox_brush_event_filter()
            self._apply_checkbox_brush_to(checkbox)
        except Exception:
            self._cancel_checkbox_brush()
            raise
        return True

    def _end_checkbox_brush(self) -> None:
        if not self._checkbox_brush_active and not self._checkbox_brush_event_filter_installed:
            return
        changed = self._checkbox_brush_changed
        try:
            self._remove_checkbox_brush_event_filter()
        finally:
            preserved_view_layer_name = self._reset_checkbox_brush_state()
        if preserved_view_layer_name:
            self._selected_view_layer_name = preserved_view_layer_name
        if changed:
            self._tag_blender_ui_redraw()
            self._push_checkbox_brush_undo()
            self._queue_refresh_from_blender()

    @context_window
    def _push_checkbox_brush_undo(self) -> None:
        bpy.ops.ed.undo_push(message="ViewLayer Manager: Brush Toggle")

    def _on_brush_checkbox_toggled(self, checked: bool) -> None:
        checkbox = self.sender()
        if not isinstance(checkbox, BrushCheckBox):
            return
        if self._checkbox_brush_active:
            return
        self._apply_checkbox_metadata(checkbox, checked, push_undo=True, refresh=True)

    def _queue_refresh_from_blender(self) -> None:
        if self._refresh_from_blender_queued:
            return
        self._refresh_from_blender_queued = True

        def _run() -> None:
            self._refresh_from_blender_queued = False
            if not self.isVisible():
                return
            self.refresh_from_blender()

        QtCore.QTimer.singleShot(0, _run)

    def _current_active_view_layer_name(self) -> str:
        window = self._blender_window()
        if window is not None and getattr(window, "view_layer", None) is not None:
            return window.view_layer.name
        active_view_layer = getattr(bpy.context, "view_layer", None)
        return active_view_layer.name if active_view_layer is not None else ""

    @context_window
    def _tag_blender_ui_redraw(self) -> None:
        window_manager = bpy.context.window_manager
        for window in window_manager.windows:
            screen = window.screen
            if screen is None:
                continue
            for area in screen.areas:
                area.tag_redraw()
                for region in area.regions:
                    region.tag_redraw()
                    if getattr(region, "type", None) == 'TEMPORARY' and hasattr(region, "tag_refresh_ui"):
                        region.tag_refresh_ui()

    def _engine(self) -> str:
        return self._scene().render.engine

    def _view_layers(self):
        return self._adapter().layers()

    def _view_layer_names(self) -> list[str]:
        return self._adapter().names()

    def _find_view_layer(self, view_layer_name: str):
        return self._adapter().find(view_layer_name)

    def _selected_view_layer_names_in_ui(self) -> list[str]:
        selected_names: list[str] = []
        for row in range(self.view_layer_list.count()):
            item = self.view_layer_list.item(row)
            if item is None or not item.isSelected():
                continue
            view_layer_name = item.data(QtCore.Qt.ItemDataRole.UserRole)
            if isinstance(view_layer_name, str) and view_layer_name:
                selected_names.append(view_layer_name)
        return selected_names

    def _selected_view_layer_names_for_apply(self) -> list[str]:
        selected_names = [name for name in self._selected_view_layer_names if self._find_view_layer(name) is not None]
        if selected_names:
            return selected_names
        current_name = self._selected_view_layer_name
        if current_name and self._find_view_layer(current_name) is not None:
            return [current_name]
        return []

    def _selected_view_layer(self):
        return self._adapter().selected(
            self._selected_view_layer_name,
            self._current_active_view_layer_name(),
        )

    def _selected_pass_preset_name(self) -> str:
        current_data = self.preset_combo.currentData()
        if isinstance(current_data, str) and current_data:
            return current_data
        return self.preset_combo.currentText().strip()

    def _reload_pass_preset_combo(self, preserve_name: str = "") -> None:
        names = list_pass_presets()
        if not preserve_name:
            preserve_name = self._selected_pass_preset_name()
        self._preset_combo_refreshing = True
        self.preset_combo.blockSignals(True)
        self.preset_combo.clear()
        for name in names:
            self.preset_combo.addItem(name, name)
        if preserve_name and preserve_name in names:
            self.preset_combo.setCurrentIndex(names.index(preserve_name))
        elif names:
            self.preset_combo.setCurrentIndex(0)
        self.preset_combo.blockSignals(False)
        self._preset_combo_refreshing = False
        self._update_preset_buttons()

    def _update_preset_buttons(self, *_args) -> None:
        if self._preset_combo_refreshing:
            return
        has_target = bool(self._selected_view_layer_names_for_apply())
        has_preset = bool(self._selected_pass_preset_name())
        self.save_new_preset_button.setEnabled(has_target)
        self.update_preset_button.setEnabled(has_target and has_preset)
        self.apply_preset_button.setEnabled(has_target and has_preset)
        self.delete_preset_button.setEnabled(has_preset)

    def _show_preset_error(self, text: str) -> None:
        QtWidgets.QMessageBox.critical(self, _tr("Preset Error"), text)

    def _save_new_pass_preset(self) -> None:
        view_layer = self._selected_view_layer()
        if view_layer is None:
            return
        preset_name, accepted = QtWidgets.QInputDialog.getText(
            self,
            _tr("Save Pass Preset"),
            _tr("Preset Name"),
        )
        if not accepted:
            return
        preset_name = preset_name.strip()
        if not preset_name:
            return
        try:
            save_pass_preset(view_layer, preset_name, overwrite=False)
        except Exception as ex:
            self._show_preset_error(str(ex))
            return
        self._reload_pass_preset_combo(preserve_name=preset_name)

    def _update_selected_pass_preset(self) -> None:
        preset_name = self._selected_pass_preset_name()
        view_layer = self._selected_view_layer()
        if not preset_name or view_layer is None:
            return
        try:
            save_pass_preset(view_layer, preset_name)
        except Exception as ex:
            self._show_preset_error(str(ex))
            return
        self._reload_pass_preset_combo(preserve_name=preset_name)

    @context_window
    def _push_apply_pass_preset_undo(self) -> None:
        bpy.ops.ed.undo_push(message="ViewLayer Manager: Apply Pass Preset")

    def _apply_selected_pass_preset(self) -> None:
        preset_name = self._selected_pass_preset_name()
        target_names = self._selected_view_layer_names_for_apply()
        if not preset_name or not target_names:
            return
        try:
            preset_data = load_pass_preset(preset_name)
            view_layers = self._adapter().resolve_names(target_names)
            changed = apply_pass_preset_to_view_layers(view_layers, preset_data)
        except Exception as ex:
            self._show_preset_error(str(ex))
            return
        if changed:
            self._tag_blender_ui_redraw()
            self._push_apply_pass_preset_undo()
            self.refresh_from_blender()

    def _delete_selected_pass_preset(self) -> None:
        preset_name = self._selected_pass_preset_name()
        if not preset_name:
            return
        try:
            delete_pass_preset(preset_name)
        except Exception as ex:
            self._show_preset_error(str(ex))
            return
        self._reload_pass_preset_combo()

    def _selected_aov(self, view_layer, index: int):
        if view_layer is None:
            return None
        if 0 <= index < len(view_layer.aovs):
            return view_layer.aovs[index]
        return None

    def _selected_lightgroup(self, view_layer, index: int):
        if view_layer is None:
            return None
        if 0 <= index < len(view_layer.lightgroups):
            return view_layer.lightgroups[index]
        return None

    def _sync_status_labels(self) -> None:
        scene = self._scene()
        self.scene_label.setText(f"{_tr('Scene')}: {scene.name}")
        active_name = self._current_active_view_layer_name() or "-"
        self.active_view_layer_label.setText(f"{_tr('Active ViewLayer')}: {active_name}")
        self.engine_label.setText(f"{_tr('Engine')}: {scene.render.engine}")

    def _rename_selected_view_layer_refs(self, old_name: str, new_name: str) -> None:
        if self._selected_view_layer_name == old_name:
            self._selected_view_layer_name = new_name
        self._selected_view_layer_names = [
            new_name if view_layer_name == old_name else view_layer_name
            for view_layer_name in self._selected_view_layer_names
        ]
        if self._classic_selection_anchor_name == old_name:
            self._classic_selection_anchor_name = new_name

    def _sync_classic_view_layer_list(self) -> None:
        current_name = self._selected_view_layer_name or self._current_active_view_layer_name()
        selected_names = [
            name for name in (self._selected_view_layer_names or [current_name]) if self._find_view_layer(name) is not None
        ]
        active_name = self._current_active_view_layer_name()
        selected_row = 0

        self.view_layer_list.blockSignals(True)
        self.view_layer_list.clear()
        self._classic_row_widgets.clear()

        for index, view_layer in enumerate(self._view_layers()):
            item = QtWidgets.QListWidgetItem(view_layer.name)
            item.setData(QtCore.Qt.ItemDataRole.UserRole, view_layer.name)
            item.setSizeHint(QtCore.QSize(240, 36))
            self.view_layer_list.addItem(item)

            row_widget = ViewLayerListRowWidget(self, view_layer.name)
            row_widget.clicked.connect(self._on_classic_row_clicked)
            self.view_layer_list.setItemWidget(item, row_widget)
            self._classic_row_widgets[view_layer.name] = row_widget

            if view_layer.name == current_name:
                selected_row = index

        if self.view_layer_list.count():
            self.view_layer_list.setCurrentRow(selected_row)
            if not selected_names:
                selected_names = [self.view_layer_list.item(selected_row).data(QtCore.Qt.ItemDataRole.UserRole)]
            for row in range(self.view_layer_list.count()):
                item = self.view_layer_list.item(row)
                if item.data(QtCore.Qt.ItemDataRole.UserRole) in selected_names:
                    item.setSelected(True)
            current_item = self.view_layer_list.currentItem()
            if current_item is not None:
                self._selected_view_layer_name = current_item.data(QtCore.Qt.ItemDataRole.UserRole)
            self._selected_view_layer_names = self._selected_view_layer_names_in_ui()
            if not self._classic_selection_anchor_name:
                self._classic_selection_anchor_name = self._selected_view_layer_name

        self.view_layer_list.blockSignals(False)
        self._update_classic_row_visuals(active_name)
        self._update_classic_reorder_buttons()

    def _update_classic_row_visuals(self, active_name: str | None = None) -> None:
        active_name = active_name if active_name is not None else self._current_active_view_layer_name()
        for row in range(self.view_layer_list.count()):
            item = self.view_layer_list.item(row)
            name = item.data(QtCore.Qt.ItemDataRole.UserRole)
            view_layer = self._find_view_layer(name)
            row_widget = self.view_layer_list.itemWidget(item)
            if view_layer is None or row_widget is None:
                continue
            row_widget.sync_from_view_layer(
                view_layer,
                is_active=view_layer.name == active_name,
                is_selected=item.isSelected(),
            )

    def _update_classic_reorder_buttons(self) -> None:
        row = self.view_layer_list.currentRow()
        has_items = self.view_layer_list.count() > 0
        self.delete_view_layer_button.setEnabled(has_items and self.view_layer_list.count() > 1)
        self.move_view_layer_up_button.setEnabled(has_items and row > 0)
        self.move_view_layer_down_button.setEnabled(has_items and 0 <= row < self.view_layer_list.count() - 1)

    def _select_classic_view_layer_by_name(self, view_layer_name: str) -> None:
        for row in range(self.view_layer_list.count()):
            item = self.view_layer_list.item(row)
            if item.data(QtCore.Qt.ItemDataRole.UserRole) == view_layer_name:
                self.view_layer_list.setCurrentRow(row)
                return

    def _on_classic_row_clicked(self, view_layer_name: str, modifiers: int) -> None:
        target_row = -1
        for row in range(self.view_layer_list.count()):
            item = self.view_layer_list.item(row)
            if item.data(QtCore.Qt.ItemDataRole.UserRole) == view_layer_name:
                target_row = row
                break
        if target_row == -1:
            return

        control_modifier = bool(modifiers & QtCore.Qt.KeyboardModifier.ControlModifier.value)
        shift_modifier = bool(modifiers & QtCore.Qt.KeyboardModifier.ShiftModifier.value)
        target_item = self.view_layer_list.item(target_row)
        no_update = QtCore.QItemSelectionModel.SelectionFlag.NoUpdate

        self.view_layer_list.blockSignals(True)
        if shift_modifier and self._classic_selection_anchor_name:
            anchor_row = target_row
            for row in range(self.view_layer_list.count()):
                item = self.view_layer_list.item(row)
                if item.data(QtCore.Qt.ItemDataRole.UserRole) == self._classic_selection_anchor_name:
                    anchor_row = row
                    break
            start_row = min(anchor_row, target_row)
            end_row = max(anchor_row, target_row)
            self.view_layer_list.clearSelection()
            for row in range(start_row, end_row + 1):
                self.view_layer_list.item(row).setSelected(True)
            self.view_layer_list.setCurrentItem(target_item, no_update)
        elif control_modifier:
            if not target_item.isSelected():
                target_item.setSelected(True)
                self.view_layer_list.setCurrentItem(target_item, no_update)
                self._classic_selection_anchor_name = view_layer_name
        else:
            self.view_layer_list.clearSelection()
            target_item.setSelected(True)
            self.view_layer_list.setCurrentItem(target_item, no_update)
            self._classic_selection_anchor_name = view_layer_name
        self.view_layer_list.blockSignals(False)

        self._selected_view_layer_name = view_layer_name
        self._selected_view_layer_names = self._selected_view_layer_names_in_ui()
        self._selected_aov_index = -1
        self._selected_lightgroup_index = -1
        self.refresh_from_blender()

    def _sync_basic_group(self, view_layer) -> None:
        self.view_layer_name_edit.blockSignals(True)
        self.view_layer_use_checkbox.blockSignals(True)
        self.view_layer_deep_checkbox.blockSignals(True)
        self.view_layer_samples_spin.blockSignals(True)
        self.view_layer_name_edit.setText(view_layer.name)
        self.view_layer_use_checkbox.setChecked(view_layer.use)
        has_deep = hasattr(view_layer, "use_deep")
        self.view_layer_deep_checkbox.setVisible(has_deep)
        self.view_layer_deep_checkbox.setEnabled(has_deep)
        self.view_layer_deep_checkbox.setChecked(bool(getattr(view_layer, "use_deep", False)))
        self.view_layer_samples_spin.setValue(view_layer.samples)
        self.view_layer_samples_spin.blockSignals(False)
        self.view_layer_deep_checkbox.blockSignals(False)
        self.view_layer_use_checkbox.blockSignals(False)
        self.view_layer_name_edit.blockSignals(False)

    def _sync_classic_pass_groups(self, view_layer) -> None:
        engine = self._engine()
        self.eevee_passes_group.setVisible(engine == ENGINE_BLENDER_EEVEE)
        self.cycles_passes_group.setVisible(engine == ENGINE_CYCLES)
        for checkbox, target_path, prop_name in self._classic_pass_bindings:
            owner = resolve_target(view_layer, target_path)
            is_supported = owner is not None and (
                (engine == ENGINE_BLENDER_EEVEE and target_path in {"view_layer", "view_layer.eevee"}) or
                (engine == ENGINE_CYCLES and target_path in {"view_layer", "view_layer.cycles"})
            )
            checkbox.blockSignals(True)
            checkbox.setEnabled(is_supported)
            checkbox.setChecked(bool(getattr(owner, prop_name)) if owner is not None else False)
            checkbox.blockSignals(False)

    def _sync_cryptomatte_group(self, view_layer) -> None:
        engine = self._engine()
        is_supported_engine = engine in {ENGINE_BLENDER_EEVEE, ENGINE_CYCLES}
        has_support = is_supported_engine and hasattr(view_layer, CRYPTOMATTE_LEVELS_PROP)
        self.cryptomatte_group.setVisible(has_support)
        if not has_support:
            return

        cryptomatte_enabled = False
        for _checkbox, prop_name in self._classic_cryptomatte_bindings:
            cryptomatte_enabled |= bool(getattr(view_layer, prop_name, False))

        self.cryptomatte_levels_spin.blockSignals(True)
        self.cryptomatte_levels_spin.setEnabled(cryptomatte_enabled)
        self.cryptomatte_levels_spin.setValue(int(getattr(view_layer, CRYPTOMATTE_LEVELS_PROP)))
        self.cryptomatte_levels_spin.blockSignals(False)

    def _sync_aov_group(self, view_layer) -> None:
        self.aov_list.blockSignals(True)
        self.aov_list.clear()
        selected_row = min(self._selected_aov_index, len(view_layer.aovs) - 1)
        for aov in view_layer.aovs:
            suffix = " [Invalid]" if not aov.is_valid else ""
            self.aov_list.addItem(f"{aov.type}: {aov.name}{suffix}")
        if self.aov_list.count():
            selected_row = max(selected_row, 0)
            self.aov_list.setCurrentRow(selected_row)
            self._selected_aov_index = selected_row
        else:
            self._selected_aov_index = -1
        _update_list_item_foregrounds(self.aov_list)
        self.aov_list.blockSignals(False)
        self._refresh_selected_aov_fields(view_layer)

    def _sync_lightgroup_group(self, view_layer) -> None:
        self.lightgroup_group.setVisible(self._engine() == ENGINE_CYCLES)
        self.lightgroup_list.blockSignals(True)
        self.lightgroup_list.clear()
        selected_row = min(self._selected_lightgroup_index, len(view_layer.lightgroups) - 1)
        for lightgroup in view_layer.lightgroups:
            self.lightgroup_list.addItem(lightgroup.name)
        if self.lightgroup_list.count():
            selected_row = max(selected_row, 0)
            self.lightgroup_list.setCurrentRow(selected_row)
            self._selected_lightgroup_index = selected_row
        else:
            self._selected_lightgroup_index = -1
        _update_list_item_foregrounds(self.lightgroup_list)
        self.lightgroup_list.blockSignals(False)
        self._refresh_selected_lightgroup_fields(view_layer)

    def _sync_cycles_light_pass_group(self, view_layer) -> None:
        is_cycles = self._engine() == ENGINE_CYCLES
        cycles_view_layer = getattr(view_layer, "cycles", None)
        has_cycles_settings = is_cycles and cycles_view_layer is not None
        self.cycles_light_pass_group.setVisible(has_cycles_settings)
        if not has_cycles_settings:
            return

        has_lightgroups = len(view_layer.lightgroups) > 0
        self.use_lightgroup_light_pass_aovs_checkbox.blockSignals(True)
        self.use_lightgroup_light_pass_aovs_checkbox.setChecked(
            cycles_view_layer.use_lightgroup_light_pass_aovs
        )
        self.use_lightgroup_light_pass_aovs_checkbox.blockSignals(False)
        self.cycles_light_pass_empty_label.setVisible(not has_lightgroups)
        enabled = has_lightgroups and cycles_view_layer.use_lightgroup_light_pass_aovs
        for checkbox, prop_name in self._classic_cycles_light_pass_bindings:
            checkbox.blockSignals(True)
            checkbox.setEnabled(enabled)
            checkbox.setChecked(bool(getattr(cycles_view_layer, prop_name)))
            checkbox.blockSignals(False)

    def _refresh_selected_aov_fields(self, view_layer) -> None:
        aov = self._selected_aov(view_layer, self._selected_aov_index)
        has_aov = aov is not None
        self.aov_name_edit.setEnabled(has_aov)
        self.aov_type_combo.setEnabled(has_aov)
        self.remove_aov_button.setEnabled(has_aov)
        self.aov_name_edit.blockSignals(True)
        self.aov_type_combo.blockSignals(True)
        if not has_aov:
            self.aov_name_edit.setText("")
            self.aov_type_combo.setCurrentIndex(0)
            self.aov_warning_label.setText("")
            self.aov_type_combo.blockSignals(False)
            self.aov_name_edit.blockSignals(False)
            return
        self.aov_name_edit.setText(aov.name)
        self.aov_type_combo.setCurrentText(aov.type)
        self.aov_warning_label.setText("" if aov.is_valid else "Conflicts with another render pass with the same name")
        self.aov_type_combo.blockSignals(False)
        self.aov_name_edit.blockSignals(False)

    def _refresh_selected_lightgroup_fields(self, view_layer) -> None:
        lightgroup = self._selected_lightgroup(view_layer, self._selected_lightgroup_index)
        has_lightgroup = lightgroup is not None
        self.lightgroup_name_edit.setEnabled(has_lightgroup)
        self.remove_lightgroup_button.setEnabled(has_lightgroup)
        self.lightgroup_name_edit.blockSignals(True)
        if not has_lightgroup:
            self.lightgroup_name_edit.setText("")
            self.lightgroup_name_edit.blockSignals(False)
            return
        self.lightgroup_name_edit.setText(lightgroup.name)
        self.lightgroup_name_edit.blockSignals(False)

    def refresh_from_blender(self) -> None:
        view_layers = self._view_layers()
        self._last_active_view_layer_name = self._current_active_view_layer_name()
        self._sync_status_labels()
        self._sync_classic_view_layer_list()
        if not view_layers:
            self._update_preset_buttons()
            return
        view_layer = self._selected_view_layer()
        if view_layer is None:
            self._update_preset_buttons()
            return
        self._selected_view_layer_name = view_layer.name
        self._sync_basic_group(view_layer)
        self._sync_classic_pass_groups(view_layer)
        self._sync_cryptomatte_group(view_layer)
        self._sync_aov_group(view_layer)
        self._sync_lightgroup_group(view_layer)
        self._sync_cycles_light_pass_group(view_layer)
        self._update_preset_buttons()

    def _sync_active_view_layer_from_context(self) -> None:
        self._last_active_view_layer_name = self._current_active_view_layer_name()
        self._sync_status_labels()
        self._sync_classic_view_layer_list()

    def _queue_active_view_layer_sync(self) -> None:
        QtCore.QTimer.singleShot(0, self._sync_active_view_layer_from_context)

    def _poll_active_view_layer_state(self) -> None:
        active_name = self._current_active_view_layer_name()
        if active_name == self._last_active_view_layer_name:
            return
        self._sync_active_view_layer_from_context()

    def _register_message_bus(self) -> None:
        if self._msgbus_registered:
            return
        bpy.msgbus.subscribe_rna(
            key=(bpy.types.Window, "view_layer"),
            owner=self._msgbus_owner,
            args=(self,),
            notify=_notify_active_view_layer_changed,
        )
        self._msgbus_registered = True

    def _unregister_message_bus(self) -> None:
        if not self._msgbus_registered:
            return
        bpy.msgbus.clear_by_owner(self._msgbus_owner)
        self._msgbus_registered = False

    def showEvent(self, event) -> None:
        super().showEvent(event)
        self._register_message_bus()
        self._active_state_timer.start()
        self.refresh_from_blender()

    def focusInEvent(self, event) -> None:
        super().focusInEvent(event)
        self.refresh_from_blender()

    def eventFilter(self, watched, event) -> bool:
        if self._checkbox_brush_active:
            try:
                event_type = event.type()
                if event_type == QtCore.QEvent.Type.MouseMove:
                    buttons = getattr(event, "buttons", lambda: QtCore.Qt.MouseButton.NoButton)()
                    if not (buttons & QtCore.Qt.MouseButton.LeftButton):
                        self._end_checkbox_brush()
                        return True
                    if hasattr(event, "globalPosition"):
                        global_pos = event.globalPosition().toPoint()
                    else:
                        global_pos = QtGui.QCursor.pos()
                    self._brush_checkbox_from_global_pos(global_pos)
                    return True
                elif event_type == QtCore.QEvent.Type.MouseButtonRelease:
                    if event.button() == QtCore.Qt.MouseButton.LeftButton:
                        if hasattr(event, "globalPosition"):
                            global_pos = event.globalPosition().toPoint()
                        else:
                            global_pos = QtGui.QCursor.pos()
                        self._brush_checkbox_from_global_pos(global_pos)
                        self._end_checkbox_brush()
                        return True
                elif event_type in {
                    QtCore.QEvent.Type.WindowDeactivate,
                    QtCore.QEvent.Type.Hide,
                }:
                    self._end_checkbox_brush()
            except Exception:
                self._cancel_checkbox_brush()
                raise
        return super().eventFilter(watched, event)

    def _release_lifecycle_resources(self) -> None:
        self._end_checkbox_brush()
        self._active_state_timer.stop()
        self._unregister_message_bus()

    def shutdown(self) -> None:
        self._release_lifecycle_resources()
        self.close()
        self.deleteLater()

    def closeEvent(self, event) -> None:
        self._release_lifecycle_resources()
        super().closeEvent(event)

    @context_window
    def _apply_form_state(
        self,
        view_layer_name: str,
        state: dict[str, object],
        *,
        aov_index: int,
        lightgroup_index: int,
    ) -> bool:
        view_layer = self._find_view_layer(view_layer_name)
        if view_layer is None:
            return False

        changed = False
        new_name = state["view_layer_name"]
        if new_name and new_name != view_layer.name:
            view_layer.name = new_name
            self._rename_selected_view_layer_refs(view_layer_name, view_layer.name)
            changed = True
            view_layer_name = view_layer.name

        changed |= set_if_changed(view_layer, "use", state["view_layer_use"])
        view_layer_deep = state.get("view_layer_deep")
        if view_layer_deep is not None and hasattr(view_layer, "use_deep"):
            changed |= set_if_changed(view_layer, "use_deep", view_layer_deep)
        changed |= set_if_changed(view_layer, "samples", state["view_layer_samples"])
        changed |= set_if_changed(view_layer, CRYPTOMATTE_LEVELS_PROP, state["cryptomatte_levels"])

        changed |= apply_live_pass_state(
            view_layer,
            engine=self._engine(),
            pass_states=state["pass_states"],
            use_lightgroup_light_pass_aovs=state["use_lightgroup_light_pass_aovs"],
            cycles_light_pass_states=state["cycles_light_pass_states"],
        )

        aov = self._selected_aov(view_layer, aov_index)
        if aov is not None:
            changed |= set_if_changed(aov, "name", state["aov_name"])
            changed |= set_if_changed(aov, "type", state["aov_type"])

        lightgroup = self._selected_lightgroup(view_layer, lightgroup_index)
        if lightgroup is not None:
            new_lightgroup_name = state["lightgroup_name"]
            if new_lightgroup_name and new_lightgroup_name != lightgroup.name:
                lightgroup.name = new_lightgroup_name
                changed = True

        if not changed:
            return False

        bpy.ops.ed.undo_push(message="ViewLayer Manager: Update")
        return True

    @context_window
    def _set_view_layer_use_in_blender(
        self,
        view_layer_name: str,
        value: bool,
        *,
        push_undo: bool = True,
    ) -> bool:
        changed = self._adapter().set_property(
            view_layer_name,
            TARGET_VIEW_LAYER,
            "use",
            value,
        )
        if changed and push_undo:
            bpy.ops.ed.undo_push(message="ViewLayer Manager: Update")
        return changed

    @context_window
    def _set_view_layer_deep_in_blender(
        self,
        view_layer_name: str,
        value: bool,
        *,
        push_undo: bool = True,
    ) -> bool:
        view_layer = self._find_view_layer(view_layer_name)
        if view_layer is None or not hasattr(view_layer, "use_deep"):
            return False
        changed = self._adapter().set_property(
            view_layer_name,
            TARGET_VIEW_LAYER,
            "use_deep",
            value,
        )
        if changed and push_undo:
            bpy.ops.ed.undo_push(message="ViewLayer Manager: Update")
        return changed

    @context_window
    def _set_view_layer_pass_property_in_blender(
        self,
        view_layer_name: str,
        target_path: str,
        prop_name: str,
        value: bool,
        *,
        push_undo: bool = True,
    ) -> bool:
        changed = self._adapter().set_property(
            view_layer_name,
            target_path,
            prop_name,
            value,
        )
        if changed and push_undo:
            bpy.ops.ed.undo_push(message="ViewLayer Manager: Update")
        return changed

    @context_window
    def _set_cycles_light_pass_master_in_blender(
        self,
        view_layer_name: str,
        value: bool,
        *,
        push_undo: bool = True,
    ) -> bool:
        changed = self._adapter().set_property(
            view_layer_name,
            TARGET_VIEW_LAYER_CYCLES,
            "use_lightgroup_light_pass_aovs",
            value,
        )
        if changed and push_undo:
            bpy.ops.ed.undo_push(message="ViewLayer Manager: Update")
        return changed

    @context_window
    def _set_cycles_light_pass_property_in_blender(
        self,
        view_layer_name: str,
        prop_name: str,
        value: bool,
        *,
        push_undo: bool = True,
    ) -> bool:
        changed = self._adapter().set_property(
            view_layer_name,
            TARGET_VIEW_LAYER_CYCLES,
            prop_name,
            value,
        )
        if changed and push_undo:
            bpy.ops.ed.undo_push(message="ViewLayer Manager: Update")
        return changed

    @context_window
    def _apply_view_layer_order(self, desired_names: list[str]) -> bool:
        changed = self._adapter().reorder(desired_names)
        if changed:
            self._tag_blender_ui_redraw()
            bpy.ops.ed.undo_push(message="ViewLayer Manager: Reorder")
        return changed

    def _set_cycles_light_pass_master(self, view_layer_name: str, checked: bool) -> None:
        self._set_cycles_light_pass_master_in_blender(view_layer_name, checked)
        self.refresh_from_blender()

    def _set_view_layer_pass_property(
        self,
        view_layer_name: str,
        target_path: str,
        prop_name: str,
        checked: bool,
    ) -> None:
        self._set_view_layer_pass_property_in_blender(view_layer_name, target_path, prop_name, checked)
        self.refresh_from_blender()

    def _set_cycles_light_pass_property(self, view_layer_name: str, prop_name: str, checked: bool) -> None:
        self._set_cycles_light_pass_property_in_blender(view_layer_name, prop_name, checked)
        self.refresh_from_blender()

    def apply_to_blender(self) -> None:
        view_layer = self._selected_view_layer()
        if view_layer is None:
            return

        pass_states = []
        for checkbox, target_path, prop_name in self._classic_pass_bindings:
            if not checkbox.isVisibleTo(self):
                continue
            pass_states.append((target_path, prop_name, checkbox.isChecked()))

        cycles_light_pass_states = [
            (prop_name, checkbox.isChecked())
            for checkbox, prop_name in self._classic_cycles_light_pass_bindings
        ]

        state = {
            "view_layer_name": self.view_layer_name_edit.text().strip(),
            "view_layer_use": self.view_layer_use_checkbox.isChecked(),
            "view_layer_deep": (
                self.view_layer_deep_checkbox.isChecked()
                if self.view_layer_deep_checkbox.isVisible()
                else None
            ),
            "view_layer_samples": self.view_layer_samples_spin.value(),
            "cryptomatte_levels": self.cryptomatte_levels_spin.value(),
            "pass_states": pass_states,
            "aov_name": self.aov_name_edit.text().strip(),
            "aov_type": self.aov_type_combo.currentText(),
            "lightgroup_name": self.lightgroup_name_edit.text().strip(),
            "use_lightgroup_light_pass_aovs": self.use_lightgroup_light_pass_aovs_checkbox.isChecked(),
            "cycles_light_pass_states": cycles_light_pass_states,
        }

        self._apply_form_state(
            view_layer.name,
            state,
            aov_index=self._selected_aov_index,
            lightgroup_index=self._selected_lightgroup_index,
        )
        self.refresh_from_blender()

    def _apply_live_update(self, *_args) -> None:
        self.apply_to_blender()

    def _on_classic_row_use_toggled(self, view_layer_name: str, checked: bool) -> None:
        self._set_view_layer_use(view_layer_name, checked)

    def _toggle_classic_light_pass_master(self, checked: bool) -> None:
        if not self._selected_view_layer_name:
            return
        self._set_cycles_light_pass_master(self._selected_view_layer_name, checked)

    def _toggle_classic_light_pass_state(self, prop_name: str, checked: bool) -> None:
        if not self._selected_view_layer_name:
            return
        self._set_cycles_light_pass_property(self._selected_view_layer_name, prop_name, checked)

    def _set_view_layer_use(self, view_layer_name: str, checked: bool) -> None:
        self._set_view_layer_use_in_blender(view_layer_name, checked)
        self.refresh_from_blender()

    def _on_view_layer_selection_changed(self, row: int) -> None:
        if self._checkbox_brush_active:
            return
        item = self.view_layer_list.item(row)
        if item is None:
            return
        self._selected_view_layer_name = item.data(QtCore.Qt.ItemDataRole.UserRole)
        self._classic_selection_anchor_name = self._selected_view_layer_name
        self._selected_aov_index = -1
        self._selected_lightgroup_index = -1
        self.refresh_from_blender()

    def _on_view_layer_multi_selection_changed(self) -> None:
        if self._checkbox_brush_active:
            return
        self._selected_view_layer_names = self._selected_view_layer_names_in_ui()
        self._update_classic_row_visuals()
        self._update_preset_buttons()

    def _on_aov_selection_changed(self, row: int) -> None:
        self._selected_aov_index = row
        _update_list_item_foregrounds(self.aov_list)
        self._refresh_selected_aov_fields(self._selected_view_layer())

    def _on_lightgroup_selection_changed(self, row: int) -> None:
        self._selected_lightgroup_index = row
        _update_list_item_foregrounds(self.lightgroup_list)
        self._refresh_selected_lightgroup_fields(self._selected_view_layer())

    @context_window
    def _set_window_view_layer(self, view_layer_name: str) -> None:
        window = self._blender_window()
        if window is None:
            raise RuntimeError("No Blender window is available")
        scene = bpy.context.scene
        for view_layer in scene.view_layers:
            if view_layer.name == view_layer_name:
                window.view_layer = view_layer
                return
        raise RuntimeError(f"ViewLayer '{view_layer_name}' was not found")

    def _restore_window_view_layer_if_available(self, view_layer_name: str) -> None:
        if not view_layer_name or self._find_view_layer(view_layer_name) is None:
            return
        try:
            self._set_window_view_layer(view_layer_name)
        except RuntimeError:
            return

    def _run_preserving_active_view_layer(self, operator):
        active_name = self._current_active_view_layer_name()
        try:
            return operator()
        finally:
            self._restore_window_view_layer_if_available(active_name)

    def _run_operator_on_view_layer(self, view_layer_name: str, operator):
        active_name = self._current_active_view_layer_name()
        self._ensure_view_layer_active(view_layer_name)
        try:
            return operator()
        finally:
            self._restore_window_view_layer_if_available(active_name)

    def _ensure_view_layer_active(self, view_layer_name: str) -> None:
        view_layer = self._find_view_layer(view_layer_name)
        if view_layer is None:
            raise RuntimeError("No view layer is selected")
        self._set_window_view_layer(view_layer.name)

    @context_window
    def _op_view_layer_add(self) -> None:
        bpy.ops.scene.view_layer_add(type='NEW')

    @context_window
    def _op_view_layer_remove(self) -> None:
        bpy.ops.scene.view_layer_remove()

    @context_window
    def _op_view_layer_add_aov(self) -> None:
        bpy.ops.scene.view_layer_add_aov()

    @context_window
    def _op_view_layer_remove_aov(self) -> None:
        bpy.ops.scene.view_layer_remove_aov()

    @context_window
    def _op_view_layer_add_lightgroup(self) -> None:
        bpy.ops.scene.view_layer_add_lightgroup()

    @context_window
    def _op_view_layer_remove_lightgroup(self) -> None:
        bpy.ops.scene.view_layer_remove_lightgroup()

    @context_window
    def _op_view_layer_add_used_lightgroups(self) -> None:
        bpy.ops.scene.view_layer_add_used_lightgroups()

    @context_window
    def _op_view_layer_remove_unused_lightgroups(self) -> None:
        bpy.ops.scene.view_layer_remove_unused_lightgroups()

    def _add_view_layer(self) -> None:
        names_before = set(self._view_layer_names())
        self._run_preserving_active_view_layer(self._op_view_layer_add)
        created_names = [name for name in self._view_layer_names() if name not in names_before]
        if created_names:
            self._selected_view_layer_name = created_names[-1]
            self._selected_view_layer_names = [created_names[-1]]
            self._classic_selection_anchor_name = created_names[-1]
        else:
            self._selected_view_layer_name = self._current_active_view_layer_name()
        self.refresh_from_blender()

    def _delete_selected_view_layer(self) -> None:
        self._delete_view_layer_by_name(self._selected_view_layer_name)

    def _delete_view_layer_by_name(self, view_layer_name: str) -> None:
        if not view_layer_name:
            return
        names_before_delete = self._view_layer_names()
        if len(names_before_delete) <= 1 or view_layer_name not in names_before_delete:
            return
        delete_index = names_before_delete.index(view_layer_name)
        remaining_names = names_before_delete[:delete_index] + names_before_delete[delete_index + 1 :]
        next_selection = remaining_names[min(delete_index, len(remaining_names) - 1)] if remaining_names else ""
        self._run_operator_on_view_layer(view_layer_name, self._op_view_layer_remove)
        self._selected_view_layer_name = next_selection or self._current_active_view_layer_name()
        self._selected_view_layer_names = [
            selected_name for selected_name in self._selected_view_layer_names if selected_name != view_layer_name
        ]
        if not self._selected_view_layer_names and next_selection:
            self._selected_view_layer_names = [next_selection]
        if self._classic_selection_anchor_name == view_layer_name:
            self._classic_selection_anchor_name = next_selection
        self._selected_aov_index = -1
        self._selected_lightgroup_index = -1
        self.refresh_from_blender()

    def _move_view_layer_by_name(self, view_layer_name: str, delta: int) -> None:
        if not view_layer_name:
            return
        names = self._view_layer_names()
        if view_layer_name not in names:
            return
        from_index = names.index(view_layer_name)
        to_index = from_index + delta
        if to_index < 0 or to_index >= len(names):
            return
        moved_name = names.pop(from_index)
        names.insert(to_index, moved_name)
        self._apply_view_layer_order(names)

    def _move_selected_view_layer(self, delta: int) -> None:
        current_name = self._selected_view_layer_name
        self._move_view_layer_by_name(current_name, delta)
        self._selected_view_layer_name = current_name
        self.refresh_from_blender()

    def _add_aov(self) -> None:
        self._add_aov_for_view_layer(self._selected_view_layer_name, self)

    def _remove_aov(self) -> None:
        self._remove_aov_for_view_layer(self._selected_view_layer_name, self, self._selected_aov_index)

    def _add_aov_for_view_layer(self, view_layer_name: str, owner) -> None:
        if not view_layer_name:
            return
        self._run_operator_on_view_layer(view_layer_name, self._op_view_layer_add_aov)
        view_layer = self._find_view_layer(view_layer_name)
        if view_layer is not None:
            owner._selected_aov_index = len(view_layer.aovs) - 1
        self.refresh_from_blender()

    def _remove_aov_for_view_layer(self, view_layer_name: str, owner, index: int) -> None:
        view_layer = self._find_view_layer(view_layer_name)
        if view_layer is None:
            return
        if 0 <= index < len(view_layer.aovs):
            view_layer.active_aov_index = index
        self._run_operator_on_view_layer(view_layer_name, self._op_view_layer_remove_aov)
        owner._selected_aov_index = max(0, index - 1)
        self.refresh_from_blender()

    def _add_lightgroup(self) -> None:
        self._add_lightgroup_for_view_layer(self._selected_view_layer_name, self)

    def _remove_lightgroup(self) -> None:
        self._remove_lightgroup_for_view_layer(
            self._selected_view_layer_name,
            self,
            self._selected_lightgroup_index,
        )

    def _add_lightgroup_for_view_layer(self, view_layer_name: str, owner) -> None:
        if not view_layer_name:
            return
        self._run_operator_on_view_layer(view_layer_name, self._op_view_layer_add_lightgroup)
        view_layer = self._find_view_layer(view_layer_name)
        if view_layer is not None:
            owner._selected_lightgroup_index = len(view_layer.lightgroups) - 1
        self.refresh_from_blender()

    def _remove_lightgroup_for_view_layer(self, view_layer_name: str, owner, index: int) -> None:
        view_layer = self._find_view_layer(view_layer_name)
        if view_layer is None:
            return
        if 0 <= index < len(view_layer.lightgroups):
            view_layer.active_lightgroup_index = index
        self._run_operator_on_view_layer(view_layer_name, self._op_view_layer_remove_lightgroup)
        owner._selected_lightgroup_index = max(0, index - 1)
        self.refresh_from_blender()

    def _add_used_lightgroups(self) -> None:
        self._add_used_lightgroups_for_view_layer(self._selected_view_layer_name)

    def _remove_unused_lightgroups(self) -> None:
        self._remove_unused_lightgroups_for_view_layer(self._selected_view_layer_name)

    def _add_used_lightgroups_for_view_layer(self, view_layer_name: str) -> None:
        if not view_layer_name:
            return
        self._run_operator_on_view_layer(view_layer_name, self._op_view_layer_add_used_lightgroups)
        self.refresh_from_blender()

    def _remove_unused_lightgroups_for_view_layer(self, view_layer_name: str) -> None:
        if not view_layer_name:
            return
        self._run_operator_on_view_layer(view_layer_name, self._op_view_layer_remove_unused_lightgroups)
        self.refresh_from_blender()
