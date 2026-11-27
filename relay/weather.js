// Weather for the device's weather screen (GET /weather). Location is
// Cloudflare's geolocation of the device's public IP, as for the voice tool;
// data is Open-Meteo (no key). Small JSON, cached per place for CACHE_S.
// Pollen (US only) is Pollen.com's public forecast for the IP's ZIP code; it is
// an unofficial endpoint, so any failure just leaves pollen out.
import { placeOf } from "./voice.js";

const CACHE_S = 600;
const HOURS = 12;
const DAYS = 3;
const WEEK = 5;   // forecast screen: today + 4

const POLLEN_CACHE_S = 3600;
const pollenLevel = (i) => i < 2.5 ? "low" : i < 4.9 ? "low-med" : i < 7.3 ? "medium" : i < 9.7 ? "med-high" : "high";

export async function pollen(zip) {
  if (!/^\d{5}$/.test(zip || "")) return null;
  const url = `https://www.pollen.com/api/forecast/current/pollen/${zip}`;
  const key = new Request(url);
  let res = await caches.default.match(key);
  if (!res) {
    const up = await fetch(url, { headers: { Referer: `https://www.pollen.com/forecast/current/pollen/${zip}`,
      "User-Agent": "Mozilla/5.0", Accept: "application/json" } });
    if (!up.ok) return null;
    res = new Response(await up.text(), { headers: { "content-type": "application/json", "cache-control": `max-age=${POLLEN_CACHE_S}` } });
    await caches.default.put(key, res.clone());
  }
  try {
    const periods = (await res.json())?.Location?.periods || [];
    const today = periods.find((p) => p.Type === "Today");
    if (!today || !Number.isFinite(today.Index)) return null;
    const tmr = periods.find((p) => p.Type === "Tomorrow");
    return {
      i: Math.round(today.Index * 10) / 10,
      lvl: pollenLevel(today.Index),
      top: (today.Triggers || []).slice(0, 2).map((t) => String(t.Name).toLowerCase()),
      tmr: tmr && Number.isFinite(tmr.Index) ? Math.round(tmr.Index * 10) / 10 : null,
    };
  } catch {
    return null;
  }
}

export async function weatherSummary(req, env) {
  const loc = placeOf(req.cf, env);
  if (!Number.isFinite(loc.lat) || !Number.isFinite(loc.lon)) return { ok: false, error: "location unknown" };
  const lat = loc.lat.toFixed(2), lon = loc.lon.toFixed(2);
  const url = `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}` +
    `&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,is_day` +
    `&hourly=temperature_2m,weather_code,precipitation_probability,is_day` +
    `&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code,sunrise,sunset` +
    `&temperature_unit=${loc.us ? "fahrenheit" : "celsius"}&wind_speed_unit=${loc.us ? "mph" : "kmh"}` +
    `&timezone=auto&forecast_days=${WEEK + 1}`;
  const cache = caches.default;
  const key = new Request(url);
  let res = await cache.match(key);
  if (!res) {
    const up = await fetch(url);
    if (!up.ok) return { ok: false, error: "weather unavailable" };
    res = new Response(await up.text(), { headers: { "content-type": "application/json", "cache-control": `max-age=${CACHE_S}` } });
    await cache.put(key, res.clone());
  }
  let j;
  try { j = await res.json(); } catch { return { ok: false, error: "weather unavailable" }; }
  const r = Math.round;
  const c = j.current, h = j.hourly, d = j.daily;
  // Hourly times are local "YYYY-MM-DDTHH:00"; start at the current hour.
  let i0 = h.time.indexOf(c.time.slice(0, 13) + ":00");
  if (i0 < 0) i0 = 0;
  const hours = [];
  for (let i = i0; i < i0 + HOURS && i < h.time.length; i++) {
    hours.push({ h: Number(h.time[i].slice(11, 13)), t: r(h.temperature_2m[i]), c: h.weather_code[i],
      p: h.precipitation_probability[i] ?? 0, d: h.is_day[i] });
  }
  const wd = (s) => new Date(s + "T12:00:00Z").toLocaleDateString("en-US", { weekday: "short", timeZone: "UTC" });
  const days = [];
  for (let i = 1; i <= DAYS && i < d.time.length; i++) {
    days.push({ d: wd(d.time[i]), hi: r(d.temperature_2m_max[i]), lo: r(d.temperature_2m_min[i]), c: d.weather_code[i] });
  }
  let pol = null;
  if (loc.us) {
    try { pol = await pollen(loc.zip); } catch { pol = null; }
  }
  const week = [];
  for (let i = 0; i < WEEK && i < d.time.length; i++) {
    week.push({ d: i === 0 ? "Today" : wd(d.time[i]), hi: r(d.temperature_2m_max[i]), lo: r(d.temperature_2m_min[i]),
      c: d.weather_code[i], p: d.precipitation_probability_max[i] ?? 0 });
  }
  return {
    ok: true,
    pollen: pol,
    place: loc.city || loc.place,
    unit: loc.us ? "F" : "C",
    wind_unit: loc.us ? "mph" : "km/h",
    now: { t: r(c.temperature_2m), feels: r(c.apparent_temperature), c: c.weather_code, wind: r(c.wind_speed_10m), d: c.is_day },
    today: { hi: r(d.temperature_2m_max[0]), lo: r(d.temperature_2m_min[0]), p: d.precipitation_probability_max[0] ?? 0,
      c: d.weather_code[0], rise: d.sunrise[0].slice(11, 16), set: d.sunset[0].slice(11, 16) },
    hours,
    days,
    week,
  };
}
