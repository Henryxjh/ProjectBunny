from __future__ import annotations

import json
import os
from typing import Any, Dict, Optional

import bpy
from bpy.types import PropertyGroup


CONFIG_FILE_NAME = "Config.json"


class ConfigManager:
    """Read/write the addon's persistent Config.json.

    All methods are ``@staticmethod`` because the manager is stateless -
    config is always read from disk on demand and written immediately when
    a property changes.
    """

    @staticmethod
    def config_path() -> str:
        return os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            CONFIG_FILE_NAME,
        )

    @staticmethod
    def load_config() -> Dict[str, str]:
        path = ConfigManager.config_path()
        if not os.path.isfile(path):
            return {}
        try:
            with open(path, "r", encoding="utf-8") as handle:
                data: Any = json.load(handle)
        except (OSError, json.JSONDecodeError):
            return {}
        if not isinstance(data, dict):
            return {}
        return {
            str(key): str(value)
            for key, value in data.items()
            if isinstance(value, str)
        }

    @staticmethod
    def save_config(settings: Optional[PropertyGroup]) -> None:
        if settings is None:
            return
        data = {
            "dump_root_directory": getattr(settings, "dump_root_directory", ""),
            "selected_frameanalysis_name": getattr(settings, "selected_frameanalysis_name", ""),
        }
        try:
            with open(ConfigManager.config_path(), "w", encoding="utf-8") as handle:
                json.dump(data, handle, indent=2)
                handle.write("\n")
        except OSError:
            pass

    @staticmethod
    def on_config_property_changed(self: PropertyGroup, context: bpy.types.Context) -> None:
        del context
        ConfigManager.save_config(self)
