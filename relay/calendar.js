// Calendar: the next week of events from one or more private iCal feeds
// (secret CAL_ICS_URLS, comma-separated; e.g. Google Calendar's "Secret address
// in iCal format"). Nothing is stored; the parsed feed is cached in memory for a
// few minutes. Recurring events (RRULE, EXDATE, moved instances) are expanded.
//
//   calendarSummary(env, tz) -> { ok, tz, now, ev: [{ s, e, t, l, a, c }] }
//     s/e epoch seconds, t title, l location, a all-day, c calendar index.

const CACHE_MS = 5 * 60 * 1000;
const WINDOW_DAYS = 8;
const MAX_EVENTS = 24;   // device buffer is 6 KB
let cache = { at: 0, key: "", events: null };
let feedStatus = [];   // per feed: HTTP status or error, for diagnosing a bad link (never the link itself)

// A few Windows zone names that Outlook/Exchange feeds use.
const WIN_TZ = {
  "Central Standard Time": "America/Chicago", "Eastern Standard Time": "America/New_York",
  "Mountain Standard Time": "America/Denver", "Pacific Standard Time": "America/Los_Angeles",
  "US Mountain Standard Time": "America/Phoenix", "UTC": "UTC", "GMT Standard Time": "Europe/London",
};
const zone = (tz, fallback) => {
  if (!tz) return fallback;
  const z = WIN_TZ[tz] || tz.replace(/^\/+/, "");
  try { new Intl.DateTimeFormat("en-US", { timeZone: z }); return z; } catch { return fallback; }
};

// Offset (ms) of tz at instant t: local wall time minus UTC.
function tzOffset(t, tz) {
  const p = Object.fromEntries(new Intl.DateTimeFormat("en-US", {
    timeZone: tz, hourCycle: "h23", year: "numeric", month: "numeric", day: "numeric",
    hour: "numeric", minute: "numeric", second: "numeric",
  }).formatToParts(new Date(t)).map((x) => [x.type, x.value]));
  return Date.UTC(+p.year, +p.month - 1, +p.day, +p.hour % 24, +p.minute, +p.second) - t;
}
// Wall-clock time in tz -> epoch ms.
function wallToUtc(y, mo, d, h, mi, s, tz) {
  const guess = Date.UTC(y, mo, d, h, mi, s);
  let t = guess - tzOffset(guess, tz);
  t = guess - tzOffset(t, tz);
  return t;
}

// "20261127T090000Z" | "20261127T090000" | "20261127" -> { y, mo, d, h, mi, s, utc, date }
function parseStamp(v) {
  const m = /^(\d{4})(\d{2})(\d{2})(?:T(\d{2})(\d{2})(\d{2})?(Z)?)?$/.exec(v.trim());
  if (!m) return null;
  return { y: +m[1], mo: +m[2] - 1, d: +m[3], h: +(m[4] || 0), mi: +(m[5] || 0), s: +(m[6] || 0), utc: !!m[7], date: !m[4] };
}
function stampToMs(st, tz) {
  if (st.utc) return Date.UTC(st.y, st.mo, st.d, st.h, st.mi, st.s);
  return wallToUtc(st.y, st.mo, st.d, st.h, st.mi, st.s, tz);
}

// ISO 8601 duration "PT1H30M" / "P1D" -> ms
function duration(v) {
  const m = /^([+-])?P(?:(\d+)W)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?$/.exec(v || "");
  if (!m) return null;
  const ms = ((+m[2] || 0) * 7 * 86400 + (+m[3] || 0) * 86400 + (+m[4] || 0) * 3600 + (+m[5] || 0) * 60 + (+m[6] || 0)) * 1000;
  return m[1] === "-" ? -ms : ms;
}

function unescapeText(v) {
  return v.replace(/\\n/gi, " ").replace(/\\([,;\\])/g, "$1").trim();
}

// Titles go on device labels with a fixed glyph set: fold accents and quotes, drop the rest.
export function plain(s, max = 48) {
  return String(s || "")
    .normalize("NFKD").replace(/[\u0300-\u036f]/g, "")
    .replace(/[\u2018\u2019\u201A\u2032]/g, "'").replace(/[\u201C\u201D\u201E\u2033]/g, "'")
    .replace(/[\u2013\u2014\u2212]/g, "-").replace(/\u2026/g, "...").replace(/&/g, "+")
    .replace(/[^ 0-9A-Za-z:/+\-.,'%()]/g, " ")
    .replace(/\s+/g, " ").trim().slice(0, max);
}

// Locations: a video link becomes its service name; a street address keeps its first part.
function place(loc) {
  const v = String(loc || "").trim();
  if (!v) return "";
  const m = /(zoom\.us|meet\.google|teams\.microsoft|webex|whereby|gotomeeting)/i.exec(v);
  if (m) return { "zoom.us": "Zoom", "meet.google": "Google Meet", "teams.microsoft": "Teams" }[m[1].toLowerCase()] || plain(m[1], 20);
  if (/^https?:\/\//i.test(v)) return "Online";
  return plain(v.split(",")[0], 40);
}

// Parse the VEVENTs of one feed into raw records.
function parseIcs(text) {
  const lines = text.replace(/\r\n[ \t]/g, "").replace(/\n[ \t]/g, "").split(/\r?\n/);
  const out = [];
  let ev = null;
  for (const line of lines) {
    if (line === "BEGIN:VEVENT") { ev = { exdate: [] }; continue; }
    if (line === "END:VEVENT") { if (ev) out.push(ev); ev = null; continue; }
    if (!ev) continue;
    const c = line.indexOf(":");
    if (c < 0) continue;
    const head = line.slice(0, c), val = line.slice(c + 1);
    const [name, ...ps] = head.split(";");
    const params = Object.fromEntries(ps.map((p) => { const i = p.indexOf("="); return [p.slice(0, i).toUpperCase(), p.slice(i + 1).replace(/^"|"$/g, "")]; }));
    switch (name.toUpperCase()) {
      case "UID": ev.uid = val; break;
      case "SUMMARY": ev.title = unescapeText(val); break;
      case "LOCATION": ev.loc = unescapeText(val); break;
      case "STATUS": ev.status = val.toUpperCase(); break;
      case "DTSTART": ev.start = parseStamp(val); ev.tz = params.TZID; break;
      case "DTEND": ev.end = parseStamp(val); ev.endTz = params.TZID; break;
      case "DURATION": ev.dur = duration(val); break;
      case "RRULE": ev.rrule = Object.fromEntries(val.split(";").map((kv) => kv.split("=")).map(([k, v]) => [k.toUpperCase(), v])); break;
      case "EXDATE": for (const v of val.split(",")) { const st = parseStamp(v); if (st) ev.exdate.push({ st, tz: params.TZID }); } break;
      case "RECURRENCE-ID": ev.recur = parseStamp(val); ev.recurTz = params.TZID; break;
    }
  }
  return out;
}

const DAYS = { SU: 0, MO: 1, TU: 2, WE: 3, TH: 4, FR: 5, SA: 6 };
const civil = (n) => { const d = new Date(n * 86400000); return { y: d.getUTCFullYear(), mo: d.getUTCMonth(), d: d.getUTCDate(), dow: d.getUTCDay() }; };
const dayNum = (y, mo, d) => Math.floor(Date.UTC(y, mo, d) / 86400000);
const daysInMonth = (y, mo) => new Date(Date.UTC(y, mo + 1, 0)).getUTCDate();

// Day numbers (days since epoch, wall-clock calendar) of each occurrence of a rule, from start until limitDay.
function occurrenceDays(st, rule, limitDay) {
  const freq = rule.FREQ, interval = Math.max(1, +rule.INTERVAL || 1);
  const count = rule.COUNT ? +rule.COUNT : Infinity;
  const until = rule.UNTIL ? parseStamp(rule.UNTIL) : null;
  const untilDay = until ? dayNum(until.y, until.mo, until.d) : Infinity;
  const startDay = dayNum(st.y, st.mo, st.d);
  const byday = (rule.BYDAY || "").split(",").filter(Boolean).map((x) => {
    const m = /^([+-]?\d+)?(SU|MO|TU|WE|TH|FR|SA)$/.exec(x); return m ? { n: m[1] ? +m[1] : 0, dow: DAYS[m[2]] } : null;
  }).filter(Boolean);
  const bymd = (rule.BYMONTHDAY || "").split(",").filter(Boolean).map(Number);
  const bymonth = (rule.BYMONTH || "").split(",").filter(Boolean).map((x) => +x - 1);
  const out = [];
  let n = 0;
  const emit = (day) => {
    if (day < startDay || n >= count || day > untilDay) return false;
    n++;
    if (day <= limitDay) out.push(day);
    return true;
  };
  const end = () => n >= count;
  if (freq === "DAILY") {
    for (let day = startDay, i = 0; day <= Math.min(limitDay, untilDay) && !end() && i < 20000; day += interval, i++) {
      const c = civil(day);
      if (byday.length && !byday.some((b) => b.dow === c.dow)) continue;
      if (bymonth.length && !bymonth.includes(c.mo)) continue;
      emit(day);
    }
  } else if (freq === "WEEKLY") {
    const wkst = DAYS[rule.WKST || "MO"];
    const dows = byday.length ? byday.map((b) => b.dow) : [civil(startDay).dow];
    const week0 = startDay - ((civil(startDay).dow - wkst + 7) % 7);
    for (let w = week0, i = 0; w <= Math.min(limitDay, untilDay) && !end() && i < 3000; w += 7 * interval, i++) {
      const days = dows.map((dw) => w + ((dw - wkst + 7) % 7)).sort((a, b) => a - b);
      for (const day of days) { if (end()) break; emit(day); }
    }
  } else if (freq === "MONTHLY" || freq === "YEARLY") {
    let { y, mo } = st;
    for (let i = 0; !end() && i < 2000; i++) {
      const first = dayNum(y, mo, 1);
      if (first > Math.min(limitDay, untilDay)) break;
      if (freq === "MONTHLY" || !bymonth.length || bymonth.includes(mo)) {
        const dim = daysInMonth(y, mo);
        let days = [];
        if (bymd.length) days = bymd.map((d) => (d < 0 ? dim + d + 1 : d)).filter((d) => d >= 1 && d <= dim).map((d) => first + d - 1);
        else if (byday.length) {
          for (const b of byday) {
            const all = [];
            for (let d = 1; d <= dim; d++) if (civil(first + d - 1).dow === b.dow) all.push(first + d - 1);
            if (b.n > 0 && all[b.n - 1] != null) days.push(all[b.n - 1]);
            else if (b.n < 0 && all[all.length + b.n] != null) days.push(all[all.length + b.n]);
            else if (b.n === 0) days.push(...all);
          }
        } else if (freq === "MONTHLY" || mo === st.mo) {
          if (st.d <= dim) days.push(first + st.d - 1);
        }
        const setpos = rule.BYSETPOS ? +rule.BYSETPOS : 0;
        days.sort((a, b) => a - b);
        if (setpos) days = [setpos > 0 ? days[setpos - 1] : days[days.length + setpos]].filter((x) => x != null);
        for (const day of days) { if (end()) break; emit(day); }
      }
      if (freq === "MONTHLY") { mo += interval; while (mo > 11) { mo -= 12; y++; } }
      else if (bymonth.length) { mo++; if (mo > 11) { mo = 0; y += interval; } }
      else y += interval;
    }
  }
  return out;
}

function expand(raw, homeTz, fromMs, toMs, calIdx) {
  const overrides = new Map();     // uid|startMs -> override event (or null if cancelled)
  const masters = [];
  for (const ev of raw) {
    if (!ev.start) continue;
    if (ev.recur) {
      const tz = zone(ev.recurTz || ev.tz, homeTz);
      overrides.set(`${ev.uid}|${ev.recur.date ? dayNum(ev.recur.y, ev.recur.mo, ev.recur.d) : stampToMs(ev.recur, tz)}`, ev);
    } else masters.push(ev);
  }
  const events = [];
  const push = (ev, sMs, eMs) => {
    if (ev.status === "CANCELLED" || eMs <= fromMs || sMs >= toMs) return;
    events.push({ s: Math.round(sMs / 1000), e: Math.round(eMs / 1000), t: plain(ev.title || "Busy"), l: place(ev.loc), a: !!ev.start.date, c: calIdx });
  };
  const span = (ev, tz) => {
    const s = ev.start.date ? wallToUtc(ev.start.y, ev.start.mo, ev.start.d, 0, 0, 0, homeTz) : stampToMs(ev.start, tz);
    let e;
    if (ev.end) e = ev.end.date ? wallToUtc(ev.end.y, ev.end.mo, ev.end.d, 0, 0, 0, homeTz) : stampToMs(ev.end, zone(ev.endTz || ev.tz, homeTz));
    else if (ev.dur != null) e = s + ev.dur;
    else e = ev.start.date ? s + 86400000 : s;
    return [s, Math.max(e, s)];
  };
  for (const ev of masters) {
    const tz = zone(ev.tz, homeTz);
    const [s0, e0] = span(ev, tz);
    const len = e0 - s0;
    if (!ev.rrule) { push(ev, s0, e0); continue; }
    // Recurrence runs in the event's own wall clock, so a 9:00 meeting stays at 9:00 across DST.
    const limitDay = Math.floor((toMs + 86400000 * 2) / 86400000);
    const ex = new Set(ev.exdate.map(({ st, tz: t }) => (st.date ? `d${dayNum(st.y, st.mo, st.d)}` : stampToMs(st, zone(t || ev.tz, homeTz)))));
    for (const day of occurrenceDays(ev.start, ev.rrule, limitDay)) {
      const c = civil(day);
      const sMs = ev.start.date ? wallToUtc(c.y, c.mo, c.d, 0, 0, 0, homeTz)
        : ev.start.utc ? Date.UTC(c.y, c.mo, c.d, ev.start.h, ev.start.mi, ev.start.s)
        : wallToUtc(c.y, c.mo, c.d, ev.start.h, ev.start.mi, ev.start.s, tz);
      if (sMs + len < fromMs - 86400000) continue;
      if (ex.has(sMs) || ex.has(`d${day}`)) continue;
      const key = `${ev.uid}|${ev.start.date ? day : sMs}`;
      if (overrides.has(key)) {
        const o = overrides.get(key);
        overrides.delete(key);
        const [os, oe] = span(o, zone(o.tz, homeTz));
        push({ ...o, title: o.title ?? ev.title, loc: o.loc ?? ev.loc }, os, oe);
        continue;
      }
      push(ev, sMs, sMs + len);
    }
  }
  // Overrides whose original slot fell outside the expansion (moved into the window).
  for (const o of overrides.values()) { const [os, oe] = span(o, zone(o.tz, homeTz)); push(o, os, oe); }
  return events;
}

async function loadEvents(env, homeTz) {
  const urls = String(env.CAL_ICS_URLS || "").split(",").map((u) => u.trim()).filter((u) => /^https?:\/\//i.test(u) || /^webcal:\/\//i.test(u));
  if (!urls.length) return null;
  const key = urls.join(",") + "|" + homeTz;
  const now = Date.now();
  if (cache.events && cache.key === key && now - cache.at < CACHE_MS) return cache.events;
  const from = now - 12 * 3600000, to = now + WINDOW_DAYS * 86400000;
  const all = [];
  let okFeeds = 0;
  await Promise.all(urls.map(async (u, i) => {
    try {
      const r = await fetch(u.replace(/^webcal:/i, "https:"), { headers: { "user-agent": "desk-buddy-relay" } });
      feedStatus[i] = r.status;
      if (!r.ok) { console.log(JSON.stringify({ ev: "cal_feed", i, status: r.status })); return; }
      all.push(...expand(parseIcs(await r.text()), homeTz, from, to, i));
      okFeeds++;
    } catch (err) { console.log(JSON.stringify({ ev: "cal_feed", i, err: String(err) })); }
  }));
  if (!okFeeds) return cache.events;          // feeds down: keep serving the last good copy
  // Same event on two calendars (an invite on both): keep one.
  const seen = new Set();
  const events = all.sort((a, b) => a.s - b.s || a.e - b.e).filter((e) => {
    const k = `${e.s}|${e.t.toLowerCase()}`; if (seen.has(k)) return false; seen.add(k); return true;
  });
  cache = { at: now, key, events };
  return events;
}

export async function calendarSummary(env, tz) {
  const homeTz = zone(tz, "America/Chicago");
  const events = await loadEvents(env, homeTz);
  if (!events) {
    const parts = String(env.CAL_ICS_URLS || "").split(",").map((u) => u.trim());
    return { ok: false, error: "calendar unavailable", feeds: parts.length, status: feedStatus, shape: parts.map((u) => `${u.length}:${u.slice(0, 6).replace(/[^a-z:\/]/gi, "?")}`) };
  }
  const now = Math.floor(Date.now() / 1000);
  return { ok: true, tz: homeTz, now, ev: events.filter((e) => e.e > now).slice(0, MAX_EVENTS) };
}

// Voice tool: events for a day ("today", "tomorrow", a weekday or YYYY-MM-DD), or the next few.
export async function calendarForVoice(env, tz, when) {
  const sum = await calendarSummary(env, tz);
  if (!sum.ok) return { error: "calendar isn't connected" };
  const homeTz = sum.tz;
  const dayKey = (s) => new Date(s * 1000).toLocaleDateString("en-CA", { timeZone: homeTz });
  const fmt = (e) => {
    const t = e.a ? "all day" : new Date(e.s * 1000).toLocaleTimeString("en-US", { timeZone: homeTz, hour: "numeric", minute: "2-digit" }) +
      "-" + new Date(e.e * 1000).toLocaleTimeString("en-US", { timeZone: homeTz, hour: "numeric", minute: "2-digit" });
    const d = new Date(e.s * 1000).toLocaleDateString("en-US", { timeZone: homeTz, weekday: "short", month: "short", day: "numeric" });
    return `${d} ${t}: ${e.t}${e.l ? " (" + e.l + ")" : ""}`;
  };
  const w = String(when || "").trim().toLowerCase();
  let target = null;
  const today = dayKey(sum.now);
  if (w === "today") target = today;
  else if (w === "tomorrow") target = dayKey(sum.now + 86400);
  else if (/^\d{4}-\d{2}-\d{2}$/.test(w)) target = w;
  else {
    const i = ["sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"].findIndex((d) => w.startsWith(d.slice(0, 3)));
    if (i >= 0) for (let k = 0; k < 8; k++) { const s = sum.now + k * 86400; if (new Date(s * 1000).toLocaleDateString("en-US", { timeZone: homeTz, weekday: "long" }).toLowerCase() === ["sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"][i]) { target = dayKey(s); break; } }
  }
  const nowStr = new Date().toLocaleString("en-US", { timeZone: homeTz, weekday: "long", hour: "numeric", minute: "2-digit" });
  if (target) {
    // Include events already over today, so "what did I have this morning" works too.
    const list = (await loadEvents(env, homeTz)).filter((e) => dayKey(e.s) === target || (e.a && dayKey(e.s) <= target && dayKey(e.e - 1) >= target));
    return { now: nowStr, day: target, events: list.length ? list.map(fmt) : ["nothing scheduled"] };
  }
  const next = sum.ev.slice(0, 6);
  return { now: nowStr, next: next.length ? next.map(fmt) : ["nothing in the next week"] };
}
