"""Configure every displayed entity through Home Assistant's entity pickers."""
from copy import deepcopy
import voluptuous as vol

from homeassistant import config_entries
from homeassistant.core import callback
from homeassistant.helpers import selector

from .model import DOMAIN, COLORS, DEFAULT_TIMEZONE, default_rows, valid_panel_id
from .weather_model import DEFAULT_WEATHER_ENTITY, DEFAULT_TEMPERATURE_SENSOR


def panel_schema(values, initial=False):
    schema = {}
    if initial:
        schema[vol.Required("panel_id", default="heating")] = str
    schema[vol.Required("title", default=values.get("title", "Heating"))] = vol.All(str, vol.Length(min=1, max=28))
    schema[vol.Required("count", default=str(values.get("count", 6)))] = selector.SelectSelector(
        selector.SelectSelectorConfig(options=["4", "6"]))
    schema[vol.Required("timezone", default=values.get("timezone", DEFAULT_TIMEZONE))] = vol.All(str, vol.Length(min=1, max=95))
    return vol.Schema(schema)


def row_schema(values):
    schema = {vol.Required("enabled", default=values.get("enabled", True)): bool,
              vol.Required("label", default=values.get("label", "")): vol.All(str, vol.Length(max=28))}
    for name in ("entity", "subtract"):
        marker = vol.Optional(name, description={"suggested_value": values.get(name, "")})
        schema[marker] = selector.EntitySelector(selector.EntitySelectorConfig())
    schema[vol.Optional("unit", default=values.get("unit", ""))] = vol.All(str, vol.Length(max=8))
    schema[vol.Required("decimals", default=values.get("decimals", 1))] = vol.All(vol.Coerce(int), vol.Range(min=0, max=3))
    schema[vol.Required("color", default=values.get("color", "cyan"))] = selector.SelectSelector(
        selector.SelectSelectorConfig(options=[key for key in COLORS if key != "muted"]))
    for name in ("warning", "critical"):
        schema[vol.Optional(name, description={"suggested_value": values.get(name)})] = vol.Coerce(float)
    return vol.Schema(schema)


def weather_schema(values):
    return vol.Schema({
        vol.Required("weather_enabled", default=values.get("weather_enabled", False)): bool,
        vol.Optional("weather_entity", description={"suggested_value": values.get("weather_entity") or DEFAULT_WEATHER_ENTITY}):
            selector.EntitySelector(selector.EntitySelectorConfig(domain="weather")),
        vol.Optional("temperature_sensor", description={
            "suggested_value": values.get("temperature_sensor", DEFAULT_TEMPERATURE_SENSOR)}):
            selector.EntitySelector(selector.EntitySelectorConfig(domain="sensor")),
        vol.Required("page_seconds", default=values.get("page_seconds", 20)):
            vol.All(vol.Coerce(int), vol.Range(min=10, max=300)),
    })


class PanelConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input:
            panel_id = user_input["panel_id"].strip()
            if not valid_panel_id(panel_id):
                errors["panel_id"] = "invalid_panel_id"
            else:
                await self.async_set_unique_id(panel_id)
                self._abort_if_unique_id_configured()
                data = dict(user_input, panel_id=panel_id, rows=default_rows())
                data["count"] = int(data["count"])
                return self.async_create_entry(title=data["title"], data=data)
        return self.async_show_form(step_id="user", data_schema=panel_schema(user_input or {}, True), errors=errors)

    @staticmethod
    @callback
    def async_get_options_flow(config_entry):
        return PanelOptionsFlow()


class PanelOptionsFlow(config_entries.OptionsFlow):
    def __init__(self):
        self._slot = 0

    @property
    def settings(self):
        return self.config_entry.options or self.config_entry.data

    async def async_step_init(self, user_input=None):
        return self.async_show_menu(step_id="init", menu_options=["panel", "weather", *[f"slot_{i}" for i in range(1, 7)]])

    async def async_step_panel(self, user_input=None):
        if user_input:
            data = deepcopy(dict(self.settings))
            data.update(user_input, count=int(user_input["count"]))
            return self.async_create_entry(title="", data=data)
        return self.async_show_form(step_id="panel", data_schema=panel_schema(self.settings))

    async def async_step_weather(self, user_input=None):
        if user_input is not None:
            if user_input.get("weather_enabled") and not user_input.get("weather_entity"):
                return self.async_show_form(step_id="weather", data_schema=weather_schema(user_input),
                                            errors={"base": "weather_required"})
            data = deepcopy(dict(self.settings))
            data.update(user_input)
            data["temperature_sensor"] = user_input.get("temperature_sensor", "")
            return self.async_create_entry(title="", data=data)
        return self.async_show_form(step_id="weather", data_schema=weather_schema(self.settings))

    async def async_step_row(self, user_input=None):
        data = deepcopy(dict(self.settings))
        rows = data.get("rows", default_rows())
        if user_input is not None:
            rows[self._slot] = user_input
            data["rows"] = rows
            return self.async_create_entry(title="", data=data)
        return self.async_show_form(step_id="row", data_schema=row_schema(rows[self._slot]),
                                    description_placeholders={"slot": str(self._slot + 1)})

    async def async_step_slot_1(self, user_input=None):
        self._slot = 0
        return await self.async_step_row()

    async def async_step_slot_2(self, user_input=None):
        self._slot = 1
        return await self.async_step_row()

    async def async_step_slot_3(self, user_input=None):
        self._slot = 2
        return await self.async_step_row()

    async def async_step_slot_4(self, user_input=None):
        self._slot = 3
        return await self.async_step_row()

    async def async_step_slot_5(self, user_input=None):
        self._slot = 4
        return await self.async_step_row()

    async def async_step_slot_6(self, user_input=None):
        self._slot = 5
        return await self.async_step_row()
