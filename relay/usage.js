// Team usage summary for the device's usage screen, from xAI's Management API.
// The Management key (secret XAI_MANAGEMENT_KEY) stays here; the device only
// gets a small JSON summary. Results are cached for CACHE_S seconds.
// Note (Nov 2026): for teams on Grok Dashboard seat billing the billing
// endpoints return HTTP 500, so this only works for console-billed teams.
//
//   GET /team-usage  (device token) ->
//   { ok, month: {total_usd, by_product: [{name, usd}]}, today_usd, week_usd,
//     spend_limit_usd, spent_this_cycle_usd, updated }

const MGMT = "https://management-api.x.ai";
const CACHE_S = 600;
let cache = { at: 0, body: null };

async function mgmt(env, path, init = {}) {
  const r = await fetch(MGMT + path, {
    ...init,
    headers: {
      authorization: `Bearer ${env.XAI_MANAGEMENT_KEY}`,
      "content-type": "application/json",
      ...(init.headers || {}),
    },
  });
  const text = await r.text();
  let body;
  try { body = JSON.parse(text); } catch { body = { raw: text.slice(0, 300) }; }
  return { status: r.status, body };
}

const fmt = (d) => d.toISOString().slice(0, 19).replace("T", " ");

async function usageBy(env, team, start, end, groupBy, tz) {
  const r = await mgmt(env, `/v1/billing/teams/${team}/usage`, {
    method: "POST",
    body: JSON.stringify({
      analyticsRequest: {
        timeRange: { startTime: fmt(start), endTime: fmt(end), timezone: tz },
        timeUnit: "TIME_UNIT_NONE",
        values: [{ name: "usd", aggregation: "AGGREGATION_SUM" }],
        groupBy,
        filters: [],
      },
    }),
  });
  if (r.status !== 200) return { error: r.status, detail: r.body };
  return (r.body.timeSeries || []).map((s) => ({
    name: (s.groupLabels || s.group || []).join(" / ") || "total",
    usd: (s.dataPoints || []).reduce((a, p) => a + Number(p.values?.[0] || 0), 0),
  }));
}

export async function teamUsage(env, debug = false) {
  if (!env.XAI_MANAGEMENT_KEY) return { ok: false, error: "management key not configured" };
  if (!debug && cache.body && Date.now() - cache.at < CACHE_S * 1000) return cache.body;

  const v = await mgmt(env, "/auth/management-keys/validation");
  const team = v.body.scopeId || v.body.teamId;
  if (!team) return { ok: false, error: "could not resolve team", status: v.status };

  const tz = env.TIMEZONE || "America/Chicago";
  const now = new Date();
  const monthStart = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), 1));
  const weekStart = new Date(now.getTime() - 7 * 86400e3);
  const dayStart = new Date(now.getTime() - 24 * 3600e3);
  const end = new Date(now.getTime() + 3600e3);

  const [byProduct, week, day, limits, preview] = await Promise.all([
    usageBy(env, team, monthStart, end, ["product"], tz),
    usageBy(env, team, weekStart, end, [], tz),
    usageBy(env, team, dayStart, end, [], tz),
    mgmt(env, `/v1/billing/teams/${team}/postpaid/spending-limits`),
    mgmt(env, `/v1/billing/teams/${team}/postpaid/invoice/preview`),
  ]);

  const cents = (x) => (x == null ? null : Number(x.val ?? x) / 100);
  const sum = (arr) => (Array.isArray(arr) ? arr.reduce((a, x) => a + x.usd, 0) : null);
  const body = {
    ok: true,
    month: Array.isArray(byProduct)
      ? { total_usd: +sum(byProduct).toFixed(2),
          by_product: byProduct.sort((a, b) => b.usd - a.usd).map((p) => ({ name: p.name, usd: +p.usd.toFixed(2) })) }
      : byProduct,
    week_usd: Array.isArray(week) ? +sum(week).toFixed(2) : week,
    today_usd: Array.isArray(day) ? +sum(day).toFixed(2) : day,
    spend_limit_usd: cents(limits.body?.spendingLimits?.effectiveSl),
    spent_this_cycle_usd: cents(preview.body?.coreInvoice?.amountBeforeVat),
    updated: now.toISOString(),
  };
  if (debug) body.debug = { validation_status: v.status, limits: limits.status, preview: preview.status };
  cache = { at: Date.now(), body };
  return body;
}
