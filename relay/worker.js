// Grok desk buddy relay: ESP-Mosaico <-> your Grok Bot's routine webhook.
//
//   POST /task     device -> relay -> Grok Bot routine webhook (adds a reply callback)
//   POST /result   Grok Bot -> relay (the routine posts its spoken answer here)
//   GET  /results  device polls; returns undelivered results once, then marks them delivered
//   GET  /voice    device WebSocket, proxied to xAI realtime with the key (see voice.js)
//   GET  /usage    device: voice minutes used per day and the daily limit
//   GET  /team-usage device: team spend summary via the Management API (usage.js)
//   POST /team-usage helper (USAGE_TOKEN) -> relay: team usage numbers read on the owner's Mac
//   GET  /summary  device: one small JSON for the usage screen (voice, bot tasks, team)
//   GET  /weather  device: current conditions, next 12 hours, next 3 days (weather.js)
//   GET  /calendar device: the next week of events from CAL_ICS_URLS (calendar.js)
//   GET  /health   open liveness check (no data)
//
// Secrets (wrangler secret put): DEVICE_TOKEN, BOT_TOKEN, XAI_API_KEY,
// GROKBOT_WEBHOOK_URL, GROKBOT_WEBHOOK_KEY, optional USAGE_TOKEN.
// DEVICE_TOKEN can send tasks and read results; BOT_TOKEN can only post results.
// State lives in one SQLite-backed Durable Object so reads see writes immediately.

import { DurableObject } from "cloudflare:workers";
import { dailyLimitSeconds, handleVoice } from "./voice.js";
import { teamUsage } from "./usage.js";
import { weatherSummary } from "./weather.js";
import { calendarSummary } from "./calendar.js";
import { placeOf } from "./voice.js";

const MAX_TASK_CHARS = 2000;
const MAX_RESULT_CHARS = 2000;
const MAX_BODY_BYTES = 16 * 1024;
const TASKS_PER_HOUR = 30;          // cost guard: each task starts a Grok Bot run
const RESULTS_PER_HOUR = 120;
const KEEP_MS = 24 * 3600 * 1000;   // prune rows older than a day
const MAX_PHOTO_BYTES = 400 * 1024;
const LOOKS_PER_DAY = 150;          // cost guard for the camera tool
const DEFAULT_VISION_MODEL = "grok-4.20-0309-non-reasoning";

function b64(buf) {
  const bytes = new Uint8Array(buf);
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

// Device photo -> xAI vision model -> short spoken-style answer. The photo is
// never stored; only a per-day count is kept.
async function look(req, env, stub) {
  const len = Number(req.headers.get("content-length") || 0);
  if (len > MAX_PHOTO_BYTES) return json({ ok: false, error: "photo too large" }, 413);
  const photo = await req.arrayBuffer();
  if (!photo.byteLength || photo.byteLength > MAX_PHOTO_BYTES) return json({ ok: false, error: "bad photo" }, 400);
  const admit = await stub.admitLook(LOOKS_PER_DAY);
  if (!admit.ok) return json({ ok: false, error: "daily look limit" }, 429);
  const question = (req.headers.get("x-question") || "What do you see?").slice(0, 300);
  const r = await fetch("https://api.x.ai/v1/chat/completions", {
    method: "POST",
    headers: { authorization: `Bearer ${env.XAI_API_KEY}`, "content-type": "application/json" },
    body: JSON.stringify({
      model: env.VISION_MODEL || DEFAULT_VISION_MODEL,
      max_tokens: 220,
      messages: [
        { role: "system", content: "You are the eyes of a small desk/garage robot. Answer the user's question about "
          + "this photo from its camera in one to three short spoken sentences, no lists or markdown. If you can't "
          + "tell, say what you can see. Read any visible text if it helps." },
        { role: "user", content: [
          { type: "image_url", image_url: { url: `data:image/jpeg;base64,${b64(photo)}`, detail: "high" } },
          { type: "text", text: question },
        ] },
      ],
    }),
  });
  if (!r.ok) return json({ ok: false, error: `vision upstream ${r.status}` }, r.status === 402 ? 402 : 502);
  const j = await r.json();
  const answer = j?.choices?.[0]?.message?.content;
  if (typeof answer !== "string" || !answer.trim()) return json({ ok: false, error: "empty answer" }, 502);
  return json({ ok: true, answer: answer.trim().slice(0, 900) });
}

const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });

function bearer(req) {
  const h = req.headers.get("authorization") || "";
  return h.startsWith("Bearer ") ? h.slice(7).trim() : "";
}

// Constant-time comparison of two strings.
function safeEqual(a, b) {
  if (!a || !b) return false;
  const ea = new TextEncoder().encode(a);
  const eb = new TextEncoder().encode(b);
  let diff = ea.length ^ eb.length;
  for (let i = 0; i < Math.max(ea.length, eb.length); i++) diff |= (ea[i] ?? 0) ^ (eb[i] ?? 0);
  return diff === 0;
}

async function readJson(req) {
  const len = Number(req.headers.get("content-length") || 0);
  if (len > MAX_BODY_BYTES) throw new Error("body too large");
  const text = await req.text();
  if (text.length > MAX_BODY_BYTES) throw new Error("body too large");
  return text ? JSON.parse(text) : {};
}

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    if (url.pathname === "/health" && req.method === "GET") return json({ ok: true });

    const token = bearer(req);
    const isDevice = safeEqual(token, env.DEVICE_TOKEN);
    const isBot = safeEqual(token, env.BOT_TOKEN);
    const stub = env.RELAY.get(env.RELAY.idFromName("main"));

    if (url.pathname === "/voice") {
      if (!isDevice) return json({ ok: false, error: "unauthorized" }, 401);
      return handleVoice(req, env, ctx, stub);
    }

    if (url.pathname === "/usage" && req.method === "GET") {
      if (!isDevice) return json({ ok: false, error: "unauthorized" }, 401);
      const limitMin = dailyLimitSeconds(env) / 60;
      const rate = Number(env.USD_PER_MINUTE || "0.08");
      const u = await stub.usage();
      const usd = Object.fromEntries(Object.entries(u.minutes_used).map(([d, m]) => [d, +(m * rate).toFixed(2)]));
      return json({ ok: true, daily_limit_minutes: +limitMin.toFixed(1),
                    daily_limit_usd: +(limitMin * rate).toFixed(2), ...u, est_usd: usd });
    }

    if (url.pathname === "/team-usage" && req.method === "GET") {
      if (!isDevice) return json({ ok: false, error: "unauthorized" }, 401);
      return json(await teamUsage(env, url.searchParams.get("debug") === "1"));
    }

    if (url.pathname === "/task" && req.method === "POST") {
      if (!isDevice) return json({ ok: false, error: "unauthorized" }, 401);
      let body;
      try { body = await readJson(req); } catch { return json({ ok: false, error: "bad json" }, 400); }
      const task = String(body.task || "").trim();
      if (!task) return json({ ok: false, error: "task required" }, 400);
      if (task.length > MAX_TASK_CHARS) return json({ ok: false, error: "task too long" }, 413);
      if (!env.GROKBOT_WEBHOOK_URL || !env.GROKBOT_WEBHOOK_KEY) {
        return json({ ok: false, error: "relay not configured" }, 503);
      }
      const id = crypto.randomUUID();
      const admit = await stub.admitTask(id, task, TASKS_PER_HOUR);
      if (!admit.ok) return json({ ok: false, error: admit.error }, 429);

      const payload = {
        request_id: id,
        task,
        source: "esp-mosaico-desk-buddy",
        requested_at: new Date().toISOString(),
        reply: {
          url: `${url.origin}/result`,
          method: "POST",
          headers: { Authorization: `Bearer ${env.BOT_TOKEN}`, "Content-Type": "application/json" },
          json: { request_id: id, spoken_text: "<1-3 short spoken sentences>", status: "done" },
        },
      };
      let upstream = 0;
      try {
        const r = await fetch(env.GROKBOT_WEBHOOK_URL, {
          method: "POST",
          headers: {
            authorization: `Bearer ${env.GROKBOT_WEBHOOK_KEY}`,
            "content-type": "application/json",
            "user-agent": "grok-buddy-relay",
          },
          body: JSON.stringify(payload),
          redirect: "manual",
        });
        upstream = r.status;
      } catch {
        upstream = 0;
      }
      await stub.markTask(id, upstream);
      const ok = upstream >= 200 && upstream < 300;
      return json({ ok, id, upstream_status: upstream }, ok ? 200 : 502);
    }

    if (url.pathname === "/result" && req.method === "POST") {
      if (!isBot) return json({ ok: false, error: "unauthorized" }, 401);
      let body;
      try { body = await readJson(req); } catch { return json({ ok: false, error: "bad json" }, 400); }
      const text = String(body.spoken_text || body.text || "").trim();
      // "attention": the bot is waiting on the owner (a card to open, a choice to make).
      const status = ["done", "error", "working", "attention"].includes(String(body.status || "done").toLowerCase())
        ? String(body.status || "done").toLowerCase()
        : "done";
      if (!text && status !== "working") return json({ ok: false, error: "spoken_text required" }, 400);
      if (text.length > MAX_RESULT_CHARS) return json({ ok: false, error: "spoken_text too long" }, 413);
      const r = await stub.addResult(String(body.request_id || ""), text, status, RESULTS_PER_HOUR);
      return json(r, r.ok ? 200 : 429);
    }

    if (url.pathname === "/team-usage" && req.method === "POST") {
      if (!safeEqual(token, env.USAGE_TOKEN)) return json({ ok: false, error: "unauthorized" }, 401);
      let body;
      try { body = await readJson(req); } catch { return json({ ok: false, error: "bad json" }, 400); }
      const num = (x) => (Number.isFinite(Number(x)) ? Number(x) : null);
      const iso = (x) => (typeof x === "string" && x.length <= 40 ? x : null);
      const team = {
        month_used_usd: num(body.month_used_usd), month_limit_usd: num(body.month_limit_usd),
        month_resets: iso(body.month_resets), week_pct: num(body.week_pct), week_resets: iso(body.week_resets),
        tokens_month: num(body.tokens_month),
      };
      await stub.setTeam(JSON.stringify(team));
      return json({ ok: true });
    }

    if (url.pathname === "/summary" && req.method === "GET") {
      if (!isDevice) return json({ ok: false, error: "unauthorized" }, 401);
      const limitMin = dailyLimitSeconds(env) / 60;
      const rate = Number(env.USD_PER_MINUTE || "0.08");
      return json(await stub.summary(limitMin, rate));
    }

    if (url.pathname === "/weather" && req.method === "GET") {
      if (!isDevice) return json({ ok: false, error: "unauthorized" }, 401);
      try { return json(await weatherSummary(req, env)); } catch { return json({ ok: false, error: "weather unavailable" }, 502); }
    }

    if (url.pathname === "/calendar" && req.method === "GET") {
      if (!isDevice) return json({ ok: false, error: "unauthorized" }, 401);
      try { return json(await calendarSummary(env, placeOf(req.cf, env).tz)); } catch (err) {
        console.log(JSON.stringify({ ev: "calendar_error", err: String(err) }));
        return json({ ok: false, error: "calendar unavailable" }, 502);
      }
    }

    if (url.pathname === "/look" && req.method === "POST") {
      if (!isDevice) return json({ ok: false, error: "unauthorized" }, 401);
      return look(req, env, stub);
    }

    if (url.pathname === "/results" && req.method === "GET") {
      if (!isDevice) return json({ ok: false, error: "unauthorized" }, 401);
      return json(await stub.takeResults());
    }

    return json({ ok: false, error: "not found" }, 404);
  },
};

export class Relay extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS tasks (
      id TEXT PRIMARY KEY, task TEXT NOT NULL, created INTEGER NOT NULL, upstream INTEGER)`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS voice_usage (day TEXT PRIMARY KEY, secs INTEGER NOT NULL)`);
    this.tz = env.TIMEZONE || "America/Chicago";
    this.sql.exec(`CREATE TABLE IF NOT EXISTS results (
      seq INTEGER PRIMARY KEY AUTOINCREMENT, request_id TEXT, text TEXT, status TEXT,
      created INTEGER NOT NULL, delivered INTEGER NOT NULL DEFAULT 0)`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT NOT NULL, at INTEGER NOT NULL)`);
  }

  admitLook(perDay) {
    const day = new Date().toLocaleDateString("en-CA", { timeZone: this.tz });
    const row = this.sql.exec("SELECT v FROM kv WHERE k = 'looks'").toArray()[0];
    let st = { day, n: 0 };
    try { if (row) st = JSON.parse(row.v); } catch {}
    if (st.day !== day) st = { day, n: 0 };
    if (st.n >= perDay) return { ok: false };
    st.n += 1;
    this.sql.exec("INSERT INTO kv (k, v, at) VALUES ('looks', ?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v, at = excluded.at",
      JSON.stringify(st), Date.now());
    return { ok: true, n: st.n };
  }

  setTeam(v) {
    this.sql.exec("INSERT INTO kv (k, v, at) VALUES ('team', ?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v, at = excluded.at",
      v, Date.now());
  }

  summary(limitMin, rate) {
    const row = this.sql.exec("SELECT secs FROM voice_usage WHERE day = ?", this.today()).toArray()[0];
    const min = row ? row.secs / 60 : 0;
    const midnight = Date.now() - this.secondsSinceMidnight() * 1000;
    const team = this.sql.exec("SELECT v, at FROM kv WHERE k = 'team'").toArray()[0];
    return {
      ok: true,
      voice_min: +min.toFixed(1), voice_limit_min: +limitMin.toFixed(0), voice_usd: +(min * rate).toFixed(2),
      bot_tasks: this.countSince("tasks", midnight),
      team: team ? { ...JSON.parse(team.v), age_s: Math.round((Date.now() - team.at) / 1000) } : null,
    };
  }

  secondsSinceMidnight() {
    const p = new Intl.DateTimeFormat("en-GB", { timeZone: this.tz, hour: "2-digit", minute: "2-digit",
      second: "2-digit", hourCycle: "h23" }).formatToParts(new Date());
    const g = (t) => Number(p.find((x) => x.type === t).value);
    return g("hour") * 3600 + g("minute") * 60 + g("second");
  }

  prune(now) {
    this.sql.exec("DELETE FROM tasks WHERE created < ?", now - KEEP_MS);
    this.sql.exec("DELETE FROM results WHERE created < ?", now - KEEP_MS);
  }

  countSince(table, since) {
    return this.sql.exec(`SELECT COUNT(*) AS n FROM ${table} WHERE created >= ?`, since).one().n;
  }

  admitTask(id, task, perHour) {
    const now = Date.now();
    this.prune(now);
    if (this.countSince("tasks", now - 3600_000) >= perHour) return { ok: false, error: "hourly task limit" };
    this.sql.exec("INSERT INTO tasks (id, task, created) VALUES (?, ?, ?)", id, task, now);
    return { ok: true };
  }

  markTask(id, upstream) {
    this.sql.exec("UPDATE tasks SET upstream = ? WHERE id = ?", upstream, id);
  }

  addResult(requestId, text, status, perHour) {
    const now = Date.now();
    if (this.countSince("results", now - 3600_000) >= perHour) return { ok: false, error: "hourly result limit" };
    this.sql.exec("INSERT INTO results (request_id, text, status, created) VALUES (?, ?, ?, ?)",
      requestId, text, status, now);
    return { ok: true };
  }

  today() {
    return new Date().toLocaleDateString("en-CA", { timeZone: this.tz });
  }

  budgetRemaining(limitSec) {
    const row = this.sql.exec("SELECT secs FROM voice_usage WHERE day = ?", this.today()).toArray()[0];
    return Math.max(0, limitSec - (row ? row.secs : 0));
  }

  budgetAdd(secs) {
    this.sql.exec(
      "INSERT INTO voice_usage (day, secs) VALUES (?, ?) ON CONFLICT(day) DO UPDATE SET secs = secs + excluded.secs",
      this.today(), secs);
  }

  usage() {
    const rows = this.sql.exec("SELECT day, secs FROM voice_usage ORDER BY day DESC LIMIT 14").toArray();
    return { minutes_used: Object.fromEntries(rows.map(r => [r.day, +(r.secs / 60).toFixed(1)])) };
  }

  takeResults() {
    const rows = this.sql
      .exec("SELECT seq, request_id, text, status FROM results WHERE delivered = 0 ORDER BY seq LIMIT 5")
      .toArray();
    for (const r of rows) this.sql.exec("UPDATE results SET delivered = 1 WHERE seq = ?", r.seq);
    return { ok: true, results: rows.map(r => ({ request_id: r.request_id, text: r.text, status: r.status })) };
  }
}
