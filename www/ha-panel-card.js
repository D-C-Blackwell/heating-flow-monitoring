class HaPanelCard extends HTMLElement {
  static getStubConfig() {
    return { entity: "sensor.ha_panel_heating" };
  }

  static getConfigElement() {
    return document.createElement("ha-panel-card-editor");
  }

  constructor() {
    super();
    this._page = 0;
    this.attachShadow({ mode: "open" });
    this.shadowRoot.innerHTML = `
      <style>
        :host { display: block; }
        ha-card { background: #101b2b; color: #edf4ff; padding: 20px; overflow: hidden; }
        header { display: flex; align-items: center; gap: 12px; margin-bottom: 18px; }
        h2 { margin: 0; font: 500 22px var(--paper-font-body1_-_font-family, sans-serif); flex: 1; }
        button { color: #b8c7d8; border: 0; background: transparent; cursor: pointer; padding: 8px; border-radius: 8px; }
        button:hover { background: #223149; }
        button:focus-visible { outline: 2px solid #65d7ff; }
        .status { color: #98a7b9; font-size: 12px; }
        .grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; }
        .reading { background: #19263a; padding: 16px; border-radius: 12px; min-width: 0; text-align: left; }
        .label { color: #a7b5c8; font-size: 13px; margin-bottom: 10px; overflow-wrap: anywhere; }
        .number { font-size: clamp(22px, 6vw, 34px); font-variant-numeric: tabular-nums; overflow-wrap: anywhere; }
        .unit { font-size: 14px; color: #a7b5c8; margin-left: 6px; }
        .pages { display: flex; gap: 8px; margin-bottom: 12px; }
        .pages[hidden] { display: none; }
        .pages button { border: 1px solid #33445b; padding: 8px 14px; }
        .pages button[aria-selected="true"] { background: #263f59; color: #65d7ff; }
        .forecast { grid-column: 1 / -1; display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; }
        .forecast .reading { padding: 12px; }
        .forecast .number { font-size: 20px; }
        .message { color: #a7b5c8; line-height: 1.5; }
      </style>
      <ha-card>
        <header>
          <h2></h2><span class="status"></span>
          <button class="configure" title="Configure display entities" aria-label="Configure display entities">
            <ha-icon icon="mdi:cog-outline"></ha-icon>
          </button>
        </header>
        <nav class="pages" aria-label="Display pages" hidden>
          <button data-page="0" aria-selected="true">Readings</button>
          <button data-page="1" aria-selected="false">Weather</button>
        </nav>
        <div class="grid"></div><div class="message"></div>
      </ha-card>`;
    for (const button of this.shadowRoot.querySelectorAll("[data-page]")) {
      button.addEventListener("click", () => {
        this._page = Number(button.dataset.page);
        this._lastData = "";
        this._render();
      });
    }
    this.shadowRoot.querySelector(".configure").addEventListener("click", () => {
      history.pushState(null, "", "/config/integrations/integration/ha_panel");
      window.dispatchEvent(new Event("location-changed"));
    });
  }

  setConfig(config) {
    this._config = { ...config };
    this._lastData = "";
    this._render();
  }

  set hass(hass) {
    this._hass = hass;
    this._render();
  }

  getCardSize() {
    return 5;
  }

  getGridOptions() {
    return { columns: 12, min_columns: 6, rows: "auto" };
  }

  _render() {
    if (!this._hass || !this._config) return;
    const entity = this._config.entity;
    const state = this._hass.states[entity];
    const data = state?.attributes;
    if (!data?.weather?.enabled) this._page = 0;
    const fingerprint = JSON.stringify([state?.state, data?.title, data?.rows,
                                        data?.screen_online, data?.weather, this._page]);
    if (fingerprint === this._lastData) return;
    this._lastData = fingerprint;
    const root = this.shadowRoot;
    root.querySelector("h2").textContent = this._page === 1 ? "Weather" : data?.title || "HA Panel";
    root.querySelector(".pages").hidden = !data?.weather?.enabled;
    for (const button of root.querySelectorAll("[data-page]")) {
      button.setAttribute("aria-selected", String(Number(button.dataset.page) === this._page));
    }
    root.querySelector(".status").textContent = data?.panel_id
      ? (data.screen_online ? "Screen online" : "Screen offline") : "";
    const grid = root.querySelector(".grid");
    grid.replaceChildren();
    const message = root.querySelector(".message");
    if (!state || !data?.panel_id) {
      message.textContent = "Choose the HA Panel sensor in this card's settings.";
      return;
    }
    if (["unavailable", "unknown"].includes(state.state)) {
      message.textContent = "The Home Assistant panel integration is unavailable.";
      return;
    }
    if (this._page === 1) {
      message.textContent = "";
      this._renderWeather(grid, data.weather);
      return;
    }
    message.textContent = data.rows?.length ? "" : "Use the settings button to add readings.";
    for (const row of data.rows || []) {
      const cell = document.createElement(row.entity ? "button" : "div");
      cell.className = "reading";
      const label = document.createElement("div");
      label.className = "label";
      label.textContent = row.label;
      const number = document.createElement("span");
      number.className = "number";
      number.textContent = row.value;
      number.style.color = /^#[0-9a-f]{6}$/i.test(row.color) ? row.color : "#65d7ff";
      const unit = document.createElement("span");
      unit.className = "unit";
      unit.textContent = row.unit;
      cell.append(label, number, unit);
      if (row.entity) {
        cell.setAttribute("aria-label", row.label + " " + row.value + " " + row.unit);
        cell.addEventListener("click", () => this.dispatchEvent(new CustomEvent("hass-more-info", {
          detail: { entityId: row.entity }, bubbles: true, composed: true,
        })));
      }
      grid.append(cell);
    }
  }
  _weatherCell(labelText, valueText, unitText = "") {
    const cell = document.createElement("div");
    cell.className = "reading";
    const label = document.createElement("div");
    label.className = "label";
    label.textContent = labelText;
    const value = document.createElement("span");
    value.className = "number";
    value.textContent = valueText;
    value.style.color = "#65d7ff";
    const unit = document.createElement("span");
    unit.className = "unit";
    unit.textContent = unitText;
    cell.append(label, value, unit);
    return cell;
  }

  _renderWeather(grid, weather) {
    grid.append(
      this._weatherCell(weather.condition_text, weather.temperature, weather.unit),
      this._weatherCell("Local sensor", weather.sensor_temperature, weather.sensor_unit),
    );
    const chance = weather.rain_probability == null ? "Chance not provided" : weather.rain_probability + "% chance";
    const rain = this._weatherCell(weather.rain_summary, chance);
    rain.style.gridColumn = "1 / -1";
    rain.querySelector(".number").style.fontSize = "20px";
    grid.append(rain);
    const forecasts = document.createElement("div");
    forecasts.className = "forecast";
    const formatter = new Intl.DateTimeFormat(this._hass.locale?.language || "en",
      weather.forecast_kind === "hourly"
        ? { hour: "2-digit", minute: "2-digit", timeZone: this._hass.config.time_zone }
        : { weekday: "short", day: "numeric", timeZone: this._hass.config.time_zone });
    for (const entry of weather.forecast || []) {
      const cell = this._weatherCell(
        formatter.format(new Date(entry.time * 1000)), entry.temperature, weather.unit);
      const condition = document.createElement("div");
      condition.className = "label";
      condition.textContent = entry.condition.replaceAll("-", " ") +
        (entry.probability == null ? "" : " Â· " + entry.probability + "%");
      cell.append(condition);
      forecasts.append(cell);
    }
    grid.append(forecasts);
  }

}

class HaPanelCardEditor extends HTMLElement {
  setConfig(config) {
    this._config = { ...config };
    this._render();
  }

  set hass(hass) {
    this._hass = hass;
    this._render();
  }

  _render() {
    if (!this._config || !this._hass) return;
    if (!this._form) {
      this._form = document.createElement("ha-form");
      this._form.schema = [{
        name: "entity", required: true,
        selector: { entity: { domain: "sensor" } },
      }];
      this._form.computeLabel = () => "HA Panel sensor";
      this._form.addEventListener("value-changed", (event) => {
        this._config = { ...this._config, ...event.detail.value };
        this.dispatchEvent(new CustomEvent("config-changed", {
          detail: { config: this._config }, bubbles: true, composed: true,
        }));
      });
      this.append(this._form);
    }
    this._form.hass = this._hass;
    this._form.data = this._config;
  }
}

customElements.define("ha-panel-card", HaPanelCard);
customElements.define("ha-panel-card-editor", HaPanelCardEditor);
window.customCards = window.customCards || [];
window.customCards.push({
  type: "ha-panel-card",
  name: "HA Panel",
  description: "The same readings as your configurable ESP8266 screen.",
  preview: true,
});
