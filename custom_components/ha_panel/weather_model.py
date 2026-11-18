"""Build bounded weather snapshots from Home Assistant states and forecasts."""
from datetime import datetime

from .model import finite_number

DEFAULT_WEATHER_ENTITY = ""
DEFAULT_TEMPERATURE_SENSOR = ""
WET_CONDITIONS = {"rainy", "pouring", "lightning-rainy", "snowy-rainy"}
CONDITIONS = {
    "clear-night": "Clear night", "cloudy": "Cloudy", "exceptional": "Unusual weather",
    "fog": "Fog", "hail": "Hail", "lightning": "Thunderstorms",
    "lightning-rainy": "Thunder and rain", "partlycloudy": "Partly cloudy",
    "pouring": "Heavy rain", "rainy": "Rain", "snowy": "Snow",
    "snowy-rainy": "Sleet", "sunny": "Sunny", "windy": "Windy",
    "windy-variant": "Windy and cloudy",
}


def forecast_epoch(value):
    try:
        parsed = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            return None
        return int(parsed.timestamp())
    except (TypeError, ValueError, OverflowError):
        return None


def temperature_text(value):
    number = finite_number(value)
    if number is None:
        return "--"
    return f"{number:.1f}" if abs(number) < 1000 else f"{number:.3g}"


def build_weather(weather, sensor, forecasts, kind, now, forecast_fresh):
    available = weather is not None and weather.state not in ("unknown", "unavailable")
    attributes = weather.attributes if available else {}
    condition = weather.state if available else "unavailable"
    sensor_available = sensor is not None and sensor.state not in ("unknown", "unavailable")
    result = {
        "enabled": True,
        "available": available,
        "condition": condition,
        "condition_text": CONDITIONS.get(condition, "Weather unavailable"),
        "temperature": temperature_text(attributes.get("temperature")),
        "unit": attributes.get("temperature_unit") or "",
        "sensor_label": (sensor.name if sensor else "Local sensor")[:28],
        "sensor_temperature": temperature_text(sensor.state if sensor_available else None),
        "sensor_unit": (sensor.attributes.get("unit_of_measurement") or "") if sensor_available else "",
        "forecast_kind": kind,
        "rain_summary": "Forecast unavailable",
        "rain_probability": None,
        "next_rain": 0,
        "forecast": [],
    }
    if condition in WET_CONDITIONS:
        result["rain_summary"] = "Rain now"
    if not forecast_fresh:
        return result

    upcoming = []
    for forecast in forecasts:
        stamp = forecast_epoch(forecast.get("datetime"))
        if stamp is None:
            continue
        # An hourly forecast describes its whole hour. Daily/twice-daily
        # entries can start earlier today and still describe current weather.
        tolerance = 3600 if kind == "hourly" else 86400
        if stamp < now - tolerance:
            continue
        if kind != "hourly":
            period = datetime.fromisoformat(str(forecast["datetime"]).replace("Z", "+00:00"))
            if period.date() < datetime.fromtimestamp(now, period.tzinfo).date():
                continue
        probability = finite_number(forecast.get("precipitation_probability"))
        if probability is not None:
            probability = round(max(0, min(100, probability)))
        upcoming.append({
            "time": stamp,
            "condition": str(forecast.get("condition") or "unknown")[:24],
            "temperature": temperature_text(forecast.get("temperature")),
            "probability": probability,
            "precipitation": finite_number(forecast.get("precipitation")),
        })
    upcoming.sort(key=lambda entry: entry["time"])
    result["forecast"] = [
        {key: entry[key] for key in ("time", "condition", "temperature", "probability")}
        for entry in upcoming[:3]
    ]
    horizon = 12 * 3600 if kind == "hourly" else 24 * 3600
    relevant = [entry for entry in upcoming if entry["time"] < now + horizon]
    if not relevant:
        return result
    probabilities = [entry["probability"] for entry in relevant if entry["probability"] is not None]
    result["rain_probability"] = max(probabilities) if probabilities else None
    wet = [
        entry for entry in relevant
        if entry["condition"] not in ("snowy", "hail") and (
            entry["condition"] in WET_CONDITIONS
            or (entry["precipitation"] is not None and entry["precipitation"] > 0)
            or (entry["probability"] is not None and entry["probability"] >= 40))
    ]
    if wet:
        result["next_rain"] = next(
            (entry["time"] for entry in wet if entry["time"] > now), 0)
    if condition not in WET_CONDITIONS:
        has_rain_data = any(
            entry["condition"] in CONDITIONS or entry["probability"] is not None
            or entry["precipitation"] is not None for entry in relevant)
        if not has_rain_data:
            return result
        if kind == "hourly":
            period = "next 12h" if relevant[-1]["time"] >= now + 10 * 3600 else "next hours"
        else:
            period = "next day"
        if wet:
            confidence = result["rain_probability"]
            label = "Rain likely" if confidence is not None and confidence >= 60 else "Rain possible"
            result["rain_summary"] = f"{label} - {period}"
        elif any(entry["condition"] == "snowy" for entry in relevant):
            result["rain_summary"] = f"Snow forecast - {period}"
        elif any(entry["condition"] == "hail" for entry in relevant):
            result["rain_summary"] = f"Hail forecast - {period}"
        else:
            result["rain_summary"] = f"No rain forecast - {period}"
    return result
