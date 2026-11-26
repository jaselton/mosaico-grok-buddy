// Grok desk buddy relay: ESP-Mosaico <-> your Grok Bot's routine webhook.
//
//   POST /task     device -> relay -> Grok Bot routine webhook (adds a reply callback)
//   POST /result   Grok Bot -> relay (the routine posts its spoken answer here)
//   GET  /results  device polls; returns undelivered results once, then marks them delivered
//   GET  /voice    device WebSocket, proxied to xAI realtime with the key (see voice.js)
//   GET  /usage    device: voice minutes used per day and the daily limit
//   GET  /health   open liveness check (no data)
//
// Secrets (wrangler secret put): DEVICE_TOKEN, BOT_TOKEN, XAI_API_KEY,
// GROKBOT_WEBHOOK_URL, GROKBOT_WEBHOOK_KEY.
// DEVICE_TOKEN can send tasks and read results; BOT_TOKEN can only post results.
// State lives in one SQLite-backed Durable Object so reads see writes immediately.

import { DurableObject } from "cloudflare:workers";
import { dailyLimitSeconds, handleVoice } from "./voice.js";

const MAX_TASK_CHARS = 2000;
const MAX_RESULT_CHARS = 2000;
const MAX_BODY_BYTES = 16 * 1024;
const TASKS_PER_HOUR = 30;          // cost guard: each task starts a Grok Bot run
const RESULTS_PER_HOUR = 120;
const KEEP_MS = 24 * 3600 * 1000;   // prune rows older than a day

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
