# grok-buddy-relay

Cloudflare Worker between the desk buddy and your Grok Bot's webhook routine.
Setup steps are in the [main README](../README.md#5-grok-bot-hand-off-optional).

| Route | Caller | Purpose |
| --- | --- | --- |
| `POST /task` | device (`DEVICE_TOKEN`) | Forward `{task}` to the routine webhook with a reply callback |
| `POST /result` | Grok Bot (`BOT_TOKEN`) | Store `{request_id, spoken_text, status}` |
| `GET /results` | device (`DEVICE_TOKEN`) | Return undelivered answers once |
| `GET /health` | anyone | Liveness |

State lives in one SQLite-backed Durable Object. Limits: 30 tasks and 120
results per hour, 2000-character task and answer, 16 KB bodies, rows pruned
after a day. The bot token can only post results; the device token can only
send tasks and read results.

Secrets (`wrangler secret put`): `DEVICE_TOKEN`, `BOT_TOKEN` (via
`setup_tokens.sh`), `GROKBOT_WEBHOOK_URL`, `GROKBOT_WEBHOOK_KEY` (via
`set_webhook.sh`).

Local test: create a git-ignored `.dev.vars` with the four values and run
`wrangler dev`.
