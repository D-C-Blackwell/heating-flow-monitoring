"""Render at the server and push retained MQTT snapshots, even with no browser open."""
import asyncio
from datetime import timedelta
import json
import logging
import time

from homeassistant.components import mqtt
from homeassistant.components.sensor import SensorEntity
from homeassistant.components.weather import WeatherEntityFeature
from homeassistant.core import callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.event import async_call_later, async_track_state_change_event, async_track_time_interval

from .model import DOMAIN, DEFAULT_TIMEZONE, default_rows, render_row, display_payload
from .weather_model import DEFAULT_WEATHER_ENTITY, DEFAULT_TEMPERATURE_SENSOR, build_weather

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([PanelSensor(entry)])


class PanelSensor(SensorEntity):
    _attr_should_poll = False
    _attr_icon = "mdi:monitor-dashboard"

    def __init__(self, entry):
        self.entry = entry
        self.settings = entry.options or entry.data
        self.panel_id = entry.data["panel_id"]
        self._attr_unique_id = f"{self.panel_id}_panel"
        self._attr_name = f"HA Panel {self.panel_id}"
        self._attr_native_value = "empty"
        self._attr_extra_state_attributes = {}
        self._lock = asyncio.Lock()
        self._cancel_pending = None
        self._closing = False
        self._refresh_again = False
        self._connected = False
        self._forecast_task = None
        self._forecasts = []
        self._forecast_kind = "hourly"
        self._forecast_updated = 0
        self._forecast_attempted = 0
        self._weather_enabled = self.settings.get("weather_enabled", False)
        self._weather_entity = self.settings.get("weather_entity") or DEFAULT_WEATHER_ENTITY
        self._temperature_sensor = self.settings.get("temperature_sensor", DEFAULT_TEMPERATURE_SENSOR)
        self._attr_device_info = {"identifiers": {(DOMAIN, self.panel_id)},
                                  "name": self.settings.get("title", "HA Panel"),
                                  "manufacturer": "Custom", "model": "ESP8266 ILI9341 panel"}

    async def async_added_to_hass(self):
        await super().async_added_to_hass()
        entities = {row.get(key) for row in self.settings.get("rows", [])
                    for key in ("entity", "subtract") if row.get(key)}
        if self._weather_enabled:
            entities.add(self._weather_entity)
            if self._temperature_sensor:
                entities.add(self._temperature_sensor)
            self.async_on_remove(async_track_time_interval(
                self.hass, self._forecast_tick, timedelta(minutes=15)))
        if entities:
            self.async_on_remove(async_track_state_change_event(self.hass, entities, self._queue))
        self.async_on_remove(async_track_time_interval(self.hass, self._queue, timedelta(seconds=30)))
        self.async_on_remove(await mqtt.async_subscribe(
            self.hass, f"ha_panel/{self.panel_id}/availability", self._status))
        await self._refresh()
        self._schedule_forecast()

    @callback
    def _status(self, message):
        self._connected = message.payload == "online"
        self._queue(None)

    @callback
    def _queue(self, _event):
        if self._closing:
            return
        if self._weather_enabled and not self._forecasts and time.monotonic() - self._forecast_attempted > 60:
            self._schedule_forecast()
        if self._cancel_pending:
            return
        self._cancel_pending = async_call_later(self.hass, 0.25, self._run)

    @callback
    def _run(self, _now):
        self._cancel_pending = None
        self.entry.async_create_task(self.hass, self._refresh(), "Refresh HA panel")

    @callback
    def _forecast_tick(self, _now):
        self._schedule_forecast()

    @callback
    def _schedule_forecast(self):
        if self._closing or not self._weather_enabled:
            return
        if self._forecast_task and not self._forecast_task.done():
            return
        self._forecast_task = self.entry.async_create_task(
            self.hass, self._fetch_forecast(), "Fetch panel weather forecast")

    async def _fetch_forecast(self):
        self._forecast_attempted = time.monotonic()
        state = self.hass.states.get(self._weather_entity)
        if not state or state.state in ("unavailable", "unknown"):
            return
        features = state.attributes.get("supported_features", 0)
        choices = (
            ("hourly", WeatherEntityFeature.FORECAST_HOURLY),
            ("daily", WeatherEntityFeature.FORECAST_DAILY),
            ("twice_daily", WeatherEntityFeature.FORECAST_TWICE_DAILY),
        )
        kind = next((name for name, flag in choices if features & flag), None)
        if kind is None:
            return
        try:
            async with asyncio.timeout(6):
                response = await self.hass.services.async_call(
                    "weather", "get_forecasts",
                    {"entity_id": self._weather_entity, "type": kind},
                    blocking=True, return_response=True)
            forecast = (response or {}).get(self._weather_entity, {}).get("forecast", [])
            self._forecasts = [entry for entry in forecast[:96] if isinstance(entry, dict)]
            self._forecast_kind = kind
            self._forecast_updated = time.monotonic()
        except (HomeAssistantError, TimeoutError, TypeError) as error:
            _LOGGER.warning("Weather forecast unavailable for panel %s: %s", self.panel_id, error)
        finally:
            if not self._closing:
                self._queue(None)

    async def _refresh(self):
        if self._lock.locked():
            self._refresh_again = True
            return
        async with self._lock:
            if self._closing:
                return
            count = int(self.settings.get("count", 6))
            rows = [render_row(row, self.hass.states.get)
                    for row in self.settings.get("rows", default_rows())[:count]
                    if row.get("enabled", True)]
            title = self.settings.get("title", "Heating")[:28]
            weather = {"enabled": False}
            if self._weather_enabled:
                weather = build_weather(
                    self.hass.states.get(self._weather_entity),
                    self.hass.states.get(self._temperature_sensor) if self._temperature_sensor else None,
                    self._forecasts, self._forecast_kind, time.time(),
                    bool(self._forecast_updated and time.monotonic() - self._forecast_updated < 3600))
            page_seconds = max(10, min(300, int(self.settings.get("page_seconds", 20))))
            attributes = {"weather": weather, "page_seconds": page_seconds, "title": title, "rows": rows, "panel_id": self.panel_id,
                          "screen_online": self._connected, "slots": count}
            self._attr_native_value = "ready" if rows and all(row["available"] for row in rows) else "incomplete" if rows else "empty"
            if self._attr_extra_state_attributes != attributes:
                self._attr_extra_state_attributes = attributes
                self.async_write_ha_state()
            payload = {"weather": weather, "page_seconds": page_seconds, "v": 1, "title": title, "rows": [{key: row[key] for key in ("label", "value", "unit", "color", "entity")} for row in rows], "slots": count,
                       "timezone": self.settings.get("timezone", DEFAULT_TIMEZONE),
                       "updated": int(time.time())}
            try:
                await mqtt.async_publish(self.hass, f"ha_panel/{self.panel_id}/state",
                                         json.dumps(display_payload(payload), ensure_ascii=True, separators=(",", ":")), qos=1, retain=True)
            except HomeAssistantError as error:
                _LOGGER.warning("Panel %s could not publish; retrying on the next heartbeat: %s", self.panel_id, error)
            if self._refresh_again and not self._closing:
                self._refresh_again = False
                self._queue(None)

    async def async_will_remove_from_hass(self):
        self._closing = True
        if self._forecast_task and not self._forecast_task.done():
            self._forecast_task.cancel()
            await asyncio.gather(self._forecast_task, return_exceptions=True)
        if self._cancel_pending:
            self._cancel_pending()
        async with self._lock:
            pass
        await super().async_will_remove_from_hass()
