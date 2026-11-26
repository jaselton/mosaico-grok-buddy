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

const BUDGET_CLOSE_CODE = 4002;

// Daily voice budget: the lower of DAILY_MINUTES and DAILY_USD at USD_PER_MINUTE.
export function dailyLimitSeconds(env) {
  const byMinutes = Number(env.DAILY_MINUTES || "20") * 60;
  const rate = Number(env.USD_PER_MINUTE || "0.08");
  const byDollars = env.DAILY_USD && rate > 0 ? (Number(env.DAILY_USD) / rate) * 60 : Infinity;
  return Math.floor(Math.min(byMinutes, byDollars));
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
      safeClose(dev, 1011, "upstream unavailable");
      return new Response(null, { status: 101, webSocket: client });
    }
  } catch (err) {
    console.log(JSON.stringify({ ev: "upstream_error", err: String(err) }));
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

  dev.addEventListener("message", (e) => safeSend(upstream, e.data));
  upstream.addEventListener("message", (e) => safeSend(dev, e.data));
  dev.addEventListener("close", () => finish("device_closed"));
  dev.addEventListener("error", () => finish("device_error"));
  upstream.addEventListener("close", () => finish("upstream_closed"));
  upstream.addEventListener("error", () => finish("upstream_error"));

  return new Response(null, { status: 101, webSocket: client });
}
