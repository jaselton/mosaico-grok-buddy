# grok-buddy-relay

Cloudflare Worker between the desk buddy, xAI's realtime voice API and your
Grok Bot's webhook routine. Setup steps are in the
[main README](../README.md#2-relay-recommended-and-xai-key).

| Route | Caller | Purpose |
| --- | --- | --- |
| `GET /voice` (WebSocket) | device (`DEVICE_TOKEN`) | Proxied to `wss://api.x.ai/v1/realtime` with `XAI_API_KEY`; daily budget enforced |
| `GET /usage` | device (`DEVICE_TOKEN`) | Voice minutes used per day and the daily limit |
| `POST /task` | device (`DEVICE_TOKEN`) | Forward `{task}` to the routine webhook with a reply callback |
| `POST /result` | Grok Bot (`BOT_TOKEN`) | Store `{request_id, spoken_text, status}`; status `done`, `error` or `attention` |
| `GET /results` | device (`DEVICE_TOKEN`) | Return undelivered answers once |
| `GET /health` | anyone | Liveness |

**Voice proxy** (`voice.js`): messages pass through unchanged, so the device
speaks xAI's protocol; the relay adds the key and pins `XAI_MODEL`. Session
time is summed per local day (`TIMEZONE`) in the Durable Object; once the
daily limit (the lower of `DAILY_MINUTES` and `DAILY_USD` / `USD_PER_MINUTE`) is spent, new sessions get
`{"type":"error","error":{"code":"daily_budget_exhausted"}}` and close code
4002, and a session is cut at the remaining budget or `MAX_SESSION_SECONDS`.

**Grok Bot tasks:** 30 tasks and 120 results per hour, 2000-character task and
answer, 16 KB bodies, rows pruned after a day. The bot token can only post
results; the device token can only use voice, send tasks and read results.

Secrets (`wrangler secret put`): `DEVICE_TOKEN`, `BOT_TOKEN` (via
`setup_tokens.sh`), `XAI_API_KEY`, `GROKBOT_WEBHOOK_URL`, `GROKBOT_WEBHOOK_KEY`
(via `set_webhook.sh`).
Optional secret `HOME_LOCATION` = `lat,lon,City,ZIP` pins the weather, pollen
and voice location when IP geolocation lands on the wrong city.

Optional peer messages: set secrets `PEER_URL` (another relay that accepts
`POST /message {from, text}` with a bearer token), `PEER_TOKEN`, and optionally
`PEER_NAME` (how Grok refers to it) and `PEER_FROM` (the sender name it hears).
Grok then gets a `send_message` tool: "tell the other device good morning".

Optional calendar: set secret `CAL_ICS_URLS` to one or more private iCal feed
links, comma-separated (Google Calendar: Settings > your calendar > Integrate
calendar > "Secret address in iCal format"). The device gets a calendar screen
(`GET /calendar`, the next week with recurrences expanded) and a next-event line
under the eyes, and Grok gets a `get_calendar` tool.

Local test: create a git-ignored `.dev.vars` with those values and run
`wrangler dev`.
