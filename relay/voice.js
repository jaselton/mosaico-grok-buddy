// Voice proxy: the device speaks xAI's realtime protocol to the relay, and the
// relay forwards it to wss://api.x.ai/v1/realtime with the API key, which lives
// only here as a Worker secret. The relay also pins the model and enforces a
// daily budget of xAI session time (billed per minute).
//
// Device -> relay:  GET /voice, Upgrade: websocket, Authorization: Bearer DEVICE_TOKEN
// Messages pass through unchanged in both directions (JSON text frames).
// When the budget is spent the relay sends
//   {"type":"error","error":{"code":"daily_budget_exhausted","message":...}}
// and closes with code 4002.

import { pollen } from "./weather.js";
import { calendarForVoice } from "./calendar.js";

const BUDGET_CLOSE_CODE = 4002;

// Relay-side tools: answered here, never forwarded to the device. Location comes
// from Cloudflare's geolocation of the device's public IP (its Wi-Fi network),
// so weather and local time follow the buddy without any setup.
const RELAY_TOOLS = [
  {
    type: "function",
    name: "get_weather",
    description: "Current weather, today's forecast, the next four days and today's pollen count where the device is.",
    parameters: { type: "object", properties: {} },
  },
  {
    type: "function",
    name: "get_location",
    description: "Where the device is (city, region, country), its time zone and the current local time.",
    parameters: { type: "object", properties: {} },
  },
];

// HOME_LOCATION (secret, optional): "lat,lon,City,ZIP" overrides IP geolocation,
// which can land on the ISP's hub city instead of where the device is.
export function placeOf(cf, env) {
  const home = String(env?.HOME_LOCATION || "").split(",").map((x) => x.trim());
  if (home.length >= 3 && Number.isFinite(Number(home[0])) && Number.isFinite(Number(home[1]))) {
    return {
      place: [home[2], cf?.region, cf?.country].filter(Boolean).join(", "),
      city: home[2], zip: home[3] || cf?.postalCode,
      lat: Number(home[0]), lon: Number(home[1]),
      tz: cf?.timezone || "UTC", us: (cf?.country || "US") === "US",
    };
  }
  const parts = [cf?.city, cf?.region, cf?.country].filter(Boolean);
  return {
    place: parts.join(", ") || "unknown",
    lat: Number(cf?.latitude),
    lon: Number(cf?.longitude),
    city: cf?.city, zip: cf?.postalCode,
    tz: cf?.timezone || "UTC",
    us: cf?.country === "US",
  };
}

function localTime(tz) {
  return new Date().toLocaleString("en-US", {
    timeZone: tz, weekday: "long", month: "long", day: "numeric", hour: "numeric", minute: "2-digit",
  });
}

const WMO = (c) => c === 0 ? "clear" : c <= 2 ? "partly cloudy" : c === 3 ? "overcast" : c <= 48 ? "foggy"
  : c <= 57 ? "drizzle" : c <= 67 ? "rain" : c <= 77 ? "snow" : c <= 82 ? "rain showers" : c <= 86 ? "snow showers" : "thunderstorms";

async function weather(loc) {
  if (!Number.isFinite(loc.lat) || !Number.isFinite(loc.lon)) return { error: "location unknown" };
  const f = loc.us ? "fahrenheit" : "celsius";
  const w = loc.us ? "mph" : "kmh";
  const url = `https://api.open-meteo.com/v1/forecast?latitude=${loc.lat}&longitude=${loc.lon}` +
    `&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m` +
    `&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code` +
    `&temperature_unit=${f}&wind_speed_unit=${w}&timezone=auto&forecast_days=5`;
  try {
    const j = await (await fetch(url)).json();
    let pol = null;
    if (loc.us) {
      try { pol = await pollen(loc.zip); } catch { pol = null; }
    }
    const unit = loc.us ? "°F" : "°C";
    return {
      place: loc.place,
      now: `${Math.round(j.current.temperature_2m)}${unit}, feels like ${Math.round(j.current.apparent_temperature)}${unit}, ${WMO(j.current.weather_code)}, wind ${Math.round(j.current.wind_speed_10m)} ${w}`,
      today: `high ${Math.round(j.daily.temperature_2m_max[0])}${unit}, low ${Math.round(j.daily.temperature_2m_min[0])}${unit}, ${WMO(j.daily.weather_code[0])}, ${j.daily.precipitation_probability_max[0]}% chance of precipitation`,
      next_days: j.daily.time.slice(1).map((d, i) => {
        const k = i + 1;
        const day = new Date(d + "T12:00:00Z").toLocaleDateString("en-US", { weekday: "long", timeZone: "UTC" });
        return `${day}: high ${Math.round(j.daily.temperature_2m_max[k])}${unit}, low ${Math.round(j.daily.temperature_2m_min[k])}${unit}, ${WMO(j.daily.weather_code[k])}, ${j.daily.precipitation_probability_max[k]}% precipitation`;
      }),
      pollen: pol ? `index ${pol.i} of 12 (${pol.lvl})${pol.top.length ? ", mainly " + pol.top.join(" and ") : ""}; tomorrow ${pol.tmr ?? "unknown"}` : "unavailable",
    };
  } catch {
    return { error: "weather unavailable" };
  }
}

// Optional peer device (PEER_URL + PEER_TOKEN secrets, PEER_NAME / PEER_FROM vars):
// another voice gadget's relay that accepts POST /message {from, text} and speaks it.
function peerTool(env) {
  if (!env.PEER_URL || !env.PEER_TOKEN) return null;
  const who = env.PEER_NAME || "the other device";
  return {
    type: "function",
    name: "send_message",
    description: `Send a short spoken message to ${who}. It chimes and reads it out loud right away (between 9 pm and 7 am it waits until morning). Use when asked to tell, send or pass something on to ${who}. Pass the message as it should be said, in the user's words.`,
    parameters: { type: "object", properties: { message: { type: "string", description: "What to say, in one or two sentences." } }, required: ["message"] },
  };
}

async function sendPeer(env, args) {
  const text = String(args?.message || "").trim().slice(0, 500);
  if (!text) return { error: "empty message" };
  try {
    const r = await fetch(String(env.PEER_URL).replace(/\/$/, "") + "/message", {
      method: "POST",
      headers: { Authorization: `Bearer ${env.PEER_TOKEN}`, "content-type": "application/json" },
      body: JSON.stringify({ from: env.PEER_FROM || "", text }),
    });
    console.log(JSON.stringify({ ev: "peer_message", status: r.status }));
    if (!r.ok) return { error: "message didn't go through" };
    const j = await r.json();
    if (j.delivered) return { sent: true, status: "playing now" };
    if (j.held) return { sent: true, status: "queued: it's quiet hours there, so it plays after 7 am" };
    if (!j.online) return { sent: true, status: "queued: that device is offline, it plays when it reconnects" };
    return { sent: true, status: "queued: it's busy and plays in a moment" };
  } catch {
    return { error: "message didn't go through" };
  }
}

const CAL_TOOL = {
  type: "function",
  name: "get_calendar",
  description: "The user's calendar. With no day: the next few events. With day (today, tomorrow, a weekday, or YYYY-MM-DD): everything that day. Use for what's next, am I free at, when is, what's on today.",
  parameters: { type: "object", properties: { day: { type: "string", description: "today, tomorrow, a weekday name, or YYYY-MM-DD; omit for the next few events" } } },
};

async function runRelayTool(name, loc, env, args) {
  if (name === "get_calendar") return calendarForVoice(env, loc.tz, args?.day);
  if (name === "send_message") return sendPeer(env, args);
  if (name === "get_weather") return weather(loc);
  if (name === "get_location") return { place: loc.place, time_zone: loc.tz, local_time: localTime(loc.tz) };
  return null;
}

/* Device -> xAI: add relay tools and location context to session.update. */
function rewriteFromDevice(data, loc, tools, env) {
  if (typeof data !== "string" || !data.startsWith('{"type":"session.update"')) return data;
  try {
    const m = JSON.parse(data);
    const ss = m.session || (m.session = {});
    ss.tools = [...(ss.tools || []), ...tools];
    ss.instructions = (ss.instructions || "") +
      ` The device is in ${loc.place} (time zone ${loc.tz}); local time there is ${localTime(loc.tz)}.` +
      " For weather or location questions call get_weather or get_location." +
      (tools.some((t) => t.name === "get_calendar")
        ? " For schedule questions (what's next, am I free, when is) call get_calendar; give times in plain speech and keep it short."
        : "") +
      (tools.some((t) => t.name === "send_message")
        ? ` You can send spoken messages to ${env.PEER_NAME || "the other device"} with send_message; say in a few words that it's sent and when it will play.`
        : "");
    return JSON.stringify(m);
  } catch {
    return data;
  }
}

// Daily voice budget: the lower of DAILY_MINUTES and DAILY_USD at USD_PER_MINUTE.
export function dailyLimitSeconds(env) {
  const byMinutes = Number(env.DAILY_MINUTES || "20") * 60;
  const rate = Number(env.USD_PER_MINUTE || "0.08");
  const byDollars = env.DAILY_USD && rate > 0 ? (Number(env.DAILY_USD) / rate) * 60 : Infinity;
  return Math.floor(Math.min(byMinutes, byDollars));
}

// Why xAI refused the session, in words short enough for the device's status line.
function upstreamError(status) {
  if (status === 402) return { code: "voice_billing", message: "Grok credits used up", status };
  if (status === 401 || status === 403) return { code: "voice_key", message: "Grok API key rejected", status };
  if (status === 429) return { code: "voice_rate_limited", message: "Grok is rate limiting", status };
  return { code: "voice_unavailable", message: "Grok voice is down", status };
}

function safeSend(ws, data) {
  try { ws.send(data); } catch { /* peer already gone */ }
}

function safeClose(ws, code, reason) {
  try { ws.close(code, reason); } catch { /* already closed */ }
}

export async function handleVoice(req, env, ctx, stub) {
  if (req.headers.get("upgrade") !== "websocket") {
    return new Response("expected websocket", { status: 426 });
  }
  if (!env.XAI_API_KEY) return new Response("voice not configured", { status: 503 });

  const limitSec = dailyLimitSeconds(env);
  const maxSessionSec = Number(env.MAX_SESSION_SECONDS || "300");
  const pair = new WebSocketPair();
  const [client, dev] = [pair[0], pair[1]];
  dev.binaryType = "arraybuffer";
  dev.accept();

  const left = await stub.budgetRemaining(limitSec);
  if (left < 15) {
    safeSend(dev, JSON.stringify({
      type: "error",
      error: { code: "daily_budget_exhausted", message: "Daily voice budget used up; back tomorrow." },
    }));
    safeClose(dev, BUDGET_CLOSE_CODE, "daily budget");
    return new Response(null, { status: 101, webSocket: client });
  }

  const model = encodeURIComponent(env.XAI_MODEL || "grok-voice-3.0");
  let upstream;
  try {
    const resp = await fetch(`https://api.x.ai/v1/realtime?model=${model}`, {
      headers: { Upgrade: "websocket", Authorization: `Bearer ${env.XAI_API_KEY}` },
    });
    upstream = resp.webSocket;
    if (!upstream) {
      console.log(JSON.stringify({ ev: "upstream_failed", status: resp.status }));
      safeSend(dev, JSON.stringify({ type: "error", error: upstreamError(resp.status) }));
      safeClose(dev, 1011, "upstream unavailable");
      return new Response(null, { status: 101, webSocket: client });
    }
  } catch (err) {
    console.log(JSON.stringify({ ev: "upstream_error", err: String(err) }));
    safeSend(dev, JSON.stringify({ type: "error", error: upstreamError(0) }));
    safeClose(dev, 1011, "upstream unavailable");
    return new Response(null, { status: 101, webSocket: client });
  }
  upstream.binaryType = "arraybuffer";   // newer compat dates default to Blob
  upstream.accept();

  const started = Date.now();
  let closed = false;
  const finish = (why) => {
    if (closed) return;
    closed = true;
    clearTimeout(cap);
    const secs = Math.max(1, Math.ceil((Date.now() - started) / 1000));
    console.log(JSON.stringify({ ev: "voice_end", why, secs }));
    ctx.waitUntil(stub.budgetAdd(secs));
    safeClose(upstream, 1000, why);
    safeClose(dev, why === "budget" ? BUDGET_CLOSE_CODE : 1000, why);
  };
  // Hard stop at the session cap or when the day's remaining budget runs out.
  const cap = setTimeout(() => {
    safeSend(dev, JSON.stringify({
      type: "error",
      error: { code: "daily_budget_exhausted", message: "Voice time limit reached." },
    }));
    finish("budget");
  }, Math.min(maxSessionSec, left) * 1000);

  const loc = placeOf(req.cf, env);
  const tools = [...RELAY_TOOLS, env.CAL_ICS_URLS ? CAL_TOOL : null, peerTool(env)].filter(Boolean);
  let swallowDone = false;   // hide the tool-call turn's response.done from the device
  dev.addEventListener("message", (e) => safeSend(upstream, rewriteFromDevice(e.data, loc, tools, env)));
  upstream.addEventListener("message", async (e) => {
    const d = e.data;
    // Only tool-call messages are inspected; audio deltas pass straight through.
    if (typeof d === "string" && d.includes('"response.function_call_arguments.done"')) {
      try {
        const m = JSON.parse(d);
        if (tools.some((t) => t.name === m.name)) {
          let args = {};
          try { args = JSON.parse(m.arguments || "{}"); } catch { /* no args */ }
          const out = await runRelayTool(m.name, loc, env, args);
          safeSend(upstream, JSON.stringify({
            type: "conversation.item.create",
            item: { type: "function_call_output", call_id: m.call_id, output: JSON.stringify(out) },
          }));
          safeSend(upstream, JSON.stringify({ type: "response.create" }));
          swallowDone = true;
          return;   // handled here; the device never sees it
        }
      } catch { /* fall through and forward */ }
    }
    if (swallowDone && typeof d === "string" && d.includes('"type":"response.done"')) {
      swallowDone = false;   // the spoken follow-up gets its own response.done
      return;
    }
    safeSend(dev, d);
  });
  dev.addEventListener("close", () => finish("device_closed"));
  dev.addEventListener("error", () => finish("device_error"));
  upstream.addEventListener("close", () => finish("upstream_closed"));
  upstream.addEventListener("error", () => finish("upstream_error"));

  return new Response(null, { status: 101, webSocket: client });
}
