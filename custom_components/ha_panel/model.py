"""Pure formatting and panel calculations, shared by all display consumers."""
from math import isfinite
import re

DOMAIN = "ha_panel"
DEFAULT_TIMEZONE = "GMT0BST,M3.5.0/1,M10.5.0"
COLORS = {"cyan": "#65d7ff", "green": "#65d58a", "amber": "#ffc857",
          "red": "#ff6666", "white": "#edf4ff", "muted": "#98a7b9"}
DEFAULT_LABELS = ["Boiler flow", "Hot press flow", "Boiler return", "Hot press rtn",
                  "Boiler A-B", "Hot press delta"]


def valid_panel_id(value):
    return bool(re.fullmatch(r"[a-z0-9_-]{1,24}", value))


def default_rows():
    # Sources are deliberately empty: select your own entities in Configure.
    return [{"enabled": True, "label": label, "entity": "", "subtract": "",
             "unit": "", "decimals": 1, "color": "cyan"}
            for label in DEFAULT_LABELS]



def finite_number(value):
    try:
        number = float(value)
        return number if isfinite(number) else None
    except (TypeError, ValueError):
        return None


def render_row(config, get_state):
    """Keep unknown and incompatible readings explicit, including delta operands."""
    source_id = config.get("entity", "")
    source = get_state(source_id) if source_id else None
    label = config.get("label") or (source.name if source else "Choose entity")
    unit = config.get("unit") or (source.attributes.get("unit_of_measurement", "") if source else "")
    unit = unit or ""
    row = {"label": label[:28], "value": "--", "unit": str(unit)[:8],
           "color": COLORS["muted"], "entity": source_id, "available": False}
    if not source or source.state in ("unknown", "unavailable"):
        return row

    value = finite_number(source.state)
    subtract_id = config.get("subtract", "")
    if subtract_id:
        other = get_state(subtract_id)
        other_value = finite_number(other.state) if other else None
        if value is None or other_value is None:
            return row
        source_unit = source.attributes.get("unit_of_measurement", "")
        other_unit = other.attributes.get("unit_of_measurement", "")
        if source_unit != other_unit:
            row["value"] = "Unit mismatch"
            return row
        value -= other_value

    color = COLORS.get(config.get("color", "cyan"), COLORS["cyan"])
    if value is not None:
        decimals = max(0, min(3, int(config.get("decimals", 1))))
        # Suppress a displayed negative zero after rounding.
        value = 0.0 if abs(value) < 0.5 * 10 ** -decimals else value
        text = f"{value:.{decimals}f}"
        if len(text) > 12:
            text = f"{value:.4g}"
        warning = finite_number(config.get("warning"))
        critical = finite_number(config.get("critical"))
        if warning is not None and value >= warning:
            color = COLORS["amber"]
        if critical is not None and value >= critical:
            color = COLORS["red"]
    else:
        text = source.state[:18]
        if text == "on" and source_id.startswith("binary_sensor."):
            text, color = "ON", COLORS["red"]
        elif text == "off" and source_id.startswith("binary_sensor."):
            text, color = "OFF", COLORS["green"]
    row.update(value=text, color=color, available=True)
    return row


def display_payload(value):
    """Use the TFT's supported character set on the wire, keeping HA labels intact."""
    if isinstance(value, str):
        return "".join(character if 32 <= ord(character) < 127 or character == "\u00b0" else "?"
                       for character in value)
    if isinstance(value, list):
        return [display_payload(item) for item in value]
    if isinstance(value, dict):
        return {key: display_payload(item) for key, item in value.items()}
    return value
