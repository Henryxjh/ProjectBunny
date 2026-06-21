from __future__ import annotations

import bpy
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, IntProperty, StringProperty
from bpy.types import PropertyGroup

from .config import ConfigManager


class ZAYCHIK_PG_frameanalysis_item(PropertyGroup):
    name: StringProperty(name="Name")
    path: StringProperty(name="Path")


class ZAYCHIK_PG_settings(PropertyGroup):
    dump_root_directory: StringProperty(
        name="Migoto Directory",
        description="Root directory containing FrameAnalysis-* folders",
        subtype="DIR_PATH",
        update=ConfigManager.on_config_property_changed,
    ) # type: ignore
    selected_frameanalysis_name: StringProperty(
        name="Selected FrameAnalysis",
        description="Last selected FrameAnalysis directory name",
        default="",
        update=ConfigManager.on_config_property_changed,
    ) # type: ignore
    max_imports: IntProperty(
        name="Max Imports",
        description="Maximum number of draw calls to try importing in one run",
        default=100,
        min=1,
        max=1000,
    ) # type: ignore
    import_all_matching: BoolProperty(
        name="Import All Matching",
        description="Import every matching draw up to Max Imports instead of stopping after the first success",
        default=True,
    ) # type: ignore
    skin_source_filter: EnumProperty(
        name="Skin Source",
        description="Filter draw calls by detected pre-skinning source",
        items=(
            ("all", "All", "Import all detected draw calls"),
            ("gpu_preskinning", "Only GPU-PreSkinning", "Import draw calls whose VB was produced by an earlier UAV write"),
            ("cpu_preskinning", "Only CPU-PreSkinning", "Import draw calls with direct IA vertex data"),
        ),
        default="all",
    ) # type: ignore
    last_status: StringProperty(
        name="Status",
        description="Latest importer status",
        default="Ready",
    ) # type: ignore
    frameanalysis_items: CollectionProperty(type=ZAYCHIK_PG_frameanalysis_item) # type: ignore
    frameanalysis_index: IntProperty(
        name="FrameAnalysis Index",
        default=0,
        update=lambda self, ctx: SettingsCallbacks.on_frameanalysis_index_changed(self, ctx),
    ) # type: ignore


class SettingsCallbacks:
    """Static helpers for PropertyGroup update callbacks.

    These live in their own class (instead of inside ZAYCHIK_PG_settings)
    because Blender's ``update=`` callback must be a plain callable and
    Blender re-instantiates PropertyGroup classes; binding to a module-level
    classmethod avoids re-registration issues.
    """

    @staticmethod
    def on_frameanalysis_index_changed(self: PropertyGroup, context: bpy.types.Context) -> None:
        """Keep selected_frameanalysis_name in sync when the active index changes.

        This runs both from user UI interaction AND from programmatic index
        updates, which is fine: it is a pure property write and save_config,
        so it is safe to call from any non-draw context. It MUST NOT be called
        from a draw() handler, and indeed the UIList changes the index outside
        of draw.
        """
        del context
        items = self.frameanalysis_items
        index = self.frameanalysis_index
        if 0 <= index < len(items):
            self.selected_frameanalysis_name = items[index].name
        else:
            self.selected_frameanalysis_name = ""
        ConfigManager.save_config(self)


CLASSES = (
    ZAYCHIK_PG_frameanalysis_item,
    ZAYCHIK_PG_settings,
)


def register() -> None:
    for klass in CLASSES:
        bpy.utils.register_class(klass)


def unregister() -> None:
    for klass in reversed(CLASSES):
        try:
            bpy.utils.unregister_class(klass)
        except (RuntimeError, ValueError):
            pass
