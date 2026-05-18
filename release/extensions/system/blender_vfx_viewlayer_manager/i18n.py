# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Translation registration and helper utilities for the manager extension."""

from __future__ import annotations

from typing import Dict, Mapping

import bpy


I18N_CONTEXT_DEFAULT = bpy.app.translations.contexts.default
I18N_CONTEXT_OPERATOR = bpy.app.translations.contexts.operator_default

_DEFAULT_PACKAGE = "bl_ext.system.blender_vfx_viewlayer_manager"
_REGISTERED = False


def _translation_domain() -> str:
    package = __package__
    if package:
        return package
    return _DEFAULT_PACKAGE


def _msgkey(text: str, *, context: str = I18N_CONTEXT_DEFAULT) -> tuple[str, str]:
    return (context, text)


_COMMON_TRANSLATIONS = {
    "ViewLayer Manager": ("ViewLayer 管理器", "ViewLayer 管理器", "Gestionnaire de ViewLayer"),
    "View Layers": ("ViewLayer 列表", "ViewLayer 列表", "ViewLayers"),
    "Scene": ("场景", "場景", "Scene"),
    "Active ViewLayer": ("当前 ViewLayer", "目前 ViewLayer", "ViewLayer actif"),
    "Engine": ("引擎", "引擎", "Moteur"),
    "Close": ("关闭", "關閉", "Fermer"),
    "Basic": ("基础", "基礎", "Base"),
    "Name": ("名称", "名稱", "Nom"),
    "Type": ("类型", "類型", "Type"),
    "Samples": ("采样", "採樣", "Échantillons"),
    "Use": ("启用", "啟用", "Utiliser"),
    "Use For Rendering": ("用于渲染", "用於渲染", "Utiliser pour le rendu"),
    "Add": ("添加", "新增", "Ajouter"),
    "Delete": ("删除", "刪除", "Supprimer"),
    "Remove": ("移除", "移除", "Retirer"),
    "Up": ("上移", "上移", "Monter"),
    "Down": ("下移", "下移", "Descendre"),
    "Shader AOV": ("Shader AOV", "Shader AOV", "AOV Shader"),
    "Light Groups": ("灯光组", "燈光組", "Groupes de lumières"),
    "Cycles Light Pass AOVs": (
        "Cycles 灯光通道 AOV",
        "Cycles 燈光通道 AOV",
        "AOV de passes lumineuses Cycles",
    ),
    "Enable Light Pass AOVs": (
        "启用灯光通道 AOV",
        "啟用燈光通道 AOV",
        "Activer les AOV de passes lumineuses",
    ),
    "Add Used": ("添加已使用", "新增已使用", "Ajouter les utilisés"),
    "Remove Unused": ("移除未使用", "移除未使用", "Retirer les inutilisés"),
    "Eevee Passes": ("Eevee 通道", "Eevee 通道", "Passes Eevee"),
    "Cycles Passes": ("Cycles 通道", "Cycles 通道", "Passes Cycles"),
    "Data": ("数据", "資料", "Données"),
    "Light": ("光照", "光照", "Lumière"),
    "Shader": ("着色器", "著色器", "Shader"),
    "Cryptomatte": ("Cryptomatte", "Cryptomatte", "Cryptomatte"),
    "Object": ("对象", "物件", "Objet"),
    "Material": ("材质", "材質", "Matériau"),
    "Asset": ("资产", "資產", "Asset"),
    "Levels": ("层级", "層級", "Niveaux"),
    "Effects / Utility": ("效果 / 工具", "效果 / 工具", "Effets / Utilitaires"),
    "Additional": ("附加", "附加", "Supplémentaire"),
    "Additional (Eevee)": ("附加 (Eevee)", "附加 (Eevee)", "Supplémentaire (Eevee)"),
    "Additional (Cycles)": ("附加 (Cycles)", "附加 (Cycles)", "Supplémentaire (Cycles)"),
    "Pass Preset": ("通道预设", "通道預設", "Préréglage de passes"),
    "Save New": ("另存为新预设", "另存為新預設", "Enregistrer comme nouveau"),
    "Update": ("更新", "更新", "Mettre à jour"),
    "Apply": ("应用", "套用", "Appliquer"),
    "Save Pass Preset": ("保存通道预设", "儲存通道預設", "Enregistrer le préréglage de passes"),
    "Preset Name": ("预设名称", "預設名稱", "Nom du préréglage"),
    "Preset Error": ("预设错误", "預設錯誤", "Erreur de préréglage"),
    "Memory scales with light groups x enabled light pass AOVs": (
        "内存占用会随灯光组数量和启用的灯光通道 AOV 一起增长",
        "記憶體占用會隨燈光組數量和啟用的燈光通道 AOV 一起增長",
        "La mémoire augmente avec le nombre de groupes de lumières et les AOV de passes lumineuses activés",
    ),
    "Add light groups to enable light pass AOV outputs": (
        "添加灯光组后才能启用灯光通道 AOV 输出",
        "新增燈光組後才能啟用燈光通道 AOV 輸出",
        "Ajoutez des groupes de lumières pour activer les sorties AOV de passes lumineuses",
    ),
    "Lobe": ("瓣别", "瓣別", "Lobe"),
    "All": ("全部", "全部", "Tout"),
    "Combined": ("合并", "合併", "Combiné"),
    "Direct": ("直接", "直接", "Direct"),
    "Indirect": ("间接", "間接", "Indirect"),
    "BQt ViewLayer Manager extension is not available in this build": (
        "当前构建中不包含 BQt ViewLayer Manager 扩展",
        "目前建置中不包含 BQt ViewLayer Manager 擴充套件",
        "L'extension BQt ViewLayer Manager n'est pas incluse dans cette build",
    ),
    "Failed to enable BQt ViewLayer Manager extension": (
        "启用 BQt ViewLayer Manager 扩展失败",
        "啟用 BQt ViewLayer Manager 擴充套件失敗",
        "Impossible d'activer l'extension BQt ViewLayer Manager",
    ),
    "Enabled the bundled BQt ViewLayer Manager for this session": (
        "已为当前会话启用内置的 BQt ViewLayer Manager",
        "已為目前工作階段啟用內建的 BQt ViewLayer Manager",
        "Le BQt ViewLayer Manager intégré a été activé pour cette session",
    ),
    "Extension module does not expose show_manager()": (
        "扩展模块没有导出 show_manager()",
        "擴充套件模組沒有匯出 show_manager()",
        "Le module d'extension n'expose pas show_manager()",
    ),
    "BQt ViewLayer Manager is only bundled for Windows builds": (
        "BQt ViewLayer Manager 目前仅随 Windows 构建提供",
        "BQt ViewLayer Manager 目前僅隨 Windows 建置提供",
        "BQt ViewLayer Manager n'est fourni que pour les builds Windows",
    ),
}

_OPERATOR_TRANSLATIONS = {
    "ViewLayer Manager": ("ViewLayer 管理器", "ViewLayer 管理器", "Gestionnaire de ViewLayer"),
    "Open ViewLayer Manager": ("打开 ViewLayer 管理器", "打開 ViewLayer 管理器", "Ouvrir le gestionnaire de ViewLayer"),
    "Open the BQt ViewLayer Manager": (
        "打开 BQt ViewLayer 管理器",
        "打開 BQt ViewLayer 管理器",
        "Ouvrir le gestionnaire BQt ViewLayer",
    ),
}


def _build_locale_dictionary(locale_index: int) -> Dict[tuple[str, str], str]:
    messages: Dict[tuple[str, str], str] = {}
    for source_text, translations in _COMMON_TRANSLATIONS.items():
        messages[_msgkey(source_text)] = translations[locale_index]
    for source_text, translations in _OPERATOR_TRANSLATIONS.items():
        messages[_msgkey(source_text, context=I18N_CONTEXT_OPERATOR)] = translations[locale_index]
    return messages


_TRANSLATIONS: Dict[str, Dict[tuple[str, str], str]] = {
    "zh_HANS": _build_locale_dictionary(0),
    "zh_HANT": _build_locale_dictionary(1),
    "fr_FR": _build_locale_dictionary(2),
}


def get_translation_dictionary() -> Mapping[str, Mapping[tuple[str, str], str]]:
    return {
        locale: dict(messages)
        for locale, messages in _TRANSLATIONS.items()
    }


def add_translation_entry(
    locale: str,
    source_text: str,
    translated_text: str,
    *,
    context: str = I18N_CONTEXT_DEFAULT,
) -> None:
    locale_key = locale.strip()
    if not locale_key:
        raise ValueError("Locale cannot be empty")

    source = source_text.strip()
    if not source:
        raise ValueError("Source text cannot be empty")

    _TRANSLATIONS.setdefault(locale_key, {})[_msgkey(source, context=context)] = translated_text


def register_translations() -> None:
    global _REGISTERED

    if _REGISTERED:
        return

    domain = _translation_domain()
    try:
        bpy.app.translations.register(domain, _TRANSLATIONS)
    except ValueError:
        try:
            bpy.app.translations.unregister(domain)
        except Exception:
            pass
        bpy.app.translations.register(domain, _TRANSLATIONS)
    _REGISTERED = True


def unregister_translations() -> None:
    global _REGISTERED

    domain = _translation_domain()
    try:
        bpy.app.translations.unregister(domain)
    except Exception:
        pass
    _REGISTERED = False


def pgettext_iface(text: str, *, context: str = I18N_CONTEXT_DEFAULT) -> str:
    return bpy.app.translations.pgettext_iface(text, context)


def pgettext_tip(text: str, *, context: str = I18N_CONTEXT_DEFAULT) -> str:
    return bpy.app.translations.pgettext_tip(text, context)


def pgettext_rpt(text: str, *, context: str = I18N_CONTEXT_DEFAULT) -> str:
    return bpy.app.translations.pgettext_rpt(text, context)


__all__ = (
    "I18N_CONTEXT_DEFAULT",
    "I18N_CONTEXT_OPERATOR",
    "get_translation_dictionary",
    "add_translation_entry",
    "register_translations",
    "unregister_translations",
    "pgettext_iface",
    "pgettext_tip",
    "pgettext_rpt",
)
