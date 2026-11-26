# Grok Desk Buddy for ESP-Mosaico

Big expressive Grok Bot-style eyes on the [ESP-Mosaico](https://github.com/esp-mosaico/esp-mosaico-vibe)'s
480×480 round screen, that you can **talk to**. Say "Hey Grok" (or tap), speak,
and Grok answers through the speaker while the eyes listen, think and talk. It
reacts when you shake, bump, tilt or pet it, wakes you up with a shake-to-stop
alarm, and can hand real tasks to **your own Grok Bot agent**.

<img src="docs/eyes.png" width="240" alt="The buddy's eyes on the Mosaico screen">

> Unofficial fan project. Not affiliated with or endorsed by xAI. The eyes are
> drawn from simple geometry (`tools/render_eyes.py`) to evoke the Grok Bot look.

## What it does

- **Talk:** xAI realtime voice (`grok-voice-3.0`, pinned), server-side turn
  detection, tap while it talks to interrupt. Starts on a tap, or hands-free
  with a **"Hey Grok" wake word** you train yourself ([`wakeword/`](wakeword/)).
- **Eyes with moods:** idle glances and blinks, listening, thinking, speaking
  (bars follow your voice and Grok's), slow dots while your Grok Bot is busy.
- **Reacts to handling:** shake → dizzy spin + buzz; bump → startle; tilt → eyes
  look downhill; face-down → sleeps with the mic off; pick up → wakes; long
  press (petting) → happy squint + purr.
- **Alarms:** "Hey Grok, wake me at 6:30" sets an alarm on the board (kept across
  reboots). It rings with worried eyes, chimes and a buzz until you shake it,
  pick it up or tap, then Grok says good morning.
- **Grok Bot hand-off:** "ask Bob to…" sends a task through the relay to a
  webhook routine on your Grok Bot; when it answers, the buddy chimes and reads
  it out. The bot can also ping you (double chime) when it's waiting on you.
- **Small-speaker audio:** 16 kHz mic uplink; 24 kHz downlink upsampled 2× with a
  windowed-sinc FIR, a 300 ms jitter buffer and a presence EQ.

## Cost and keys

xAI bills voice per minute (about $0.05–0.08/min at the time of writing; see
[xAI's pricing](https://docs.x.ai/developers/models/voice-agent-api)).

- **Recommended: voice through the relay.** The [`relay/`](relay/) Cloudflare
  Worker holds your xAI key as a secret (nothing extractable on the board),
  pins the model and enforces a **daily voice budget**: the lower of
  `DAILY_MINUTES` (default 60) and `DAILY_USD` (default $10) at `USD_PER_MINUTE`;
  `GET /usage` shows minutes and estimated dollars per day. The Worker fits the
  free tier.
- **Fallback: direct.** Without a relay the board connects to xAI itself using a
  key from Vibe Mode or `secrets/xai_api_key`; anyone holding the board could
  extract it.
- The buddy only connects when spoken to, closes the connection 10 s after a
  reply, and stops listening after 5 s of silence. Grok Bot hand-offs run on
  your Grok Bot plan; the relay caps them at 30 an hour.

## Setup

### 1. Workspace and Wi-Fi

1. Set up the [esp-mosaico-vibe](https://github.com/esp-mosaico/esp-mosaico-vibe)
   workspace (ESP-IDF at the SHA it pins, Python 3.10+) and verify a build.
2. Clone this repo into the workspace's `projects/` folder as `grok_desk_buddy`.
3. On the board, enter **Vibe Mode → Settings** and save your Wi-Fi.

### 2. Relay (recommended) and xAI key

Get a key at [console.x.ai](https://console.x.ai), then (needs a Cloudflare
account and `wrangler login`):

```sh
cd relay && wrangler deploy
zsh setup_tokens.sh https://grok-buddy-relay.<your-subdomain>.workers.dev
wrangler secret put XAI_API_KEY      # paste the key at the hidden prompt
```

`setup_tokens.sh` creates the device and bot tokens and writes the board's copy
to the git-ignored `secrets/relay_url` and `secrets/relay_token`. Adjust
`DAILY_MINUTES`, `DAILY_USD`, `USD_PER_MINUTE` and `TIMEZONE` in
`relay/wrangler.toml`.

*Direct mode instead:* add a shared setting `xai_api_key` in **Vibe Mode →
Settings → Shared information**, or run `zsh tools/set_xai_key.sh`.

### 3. Personal settings (optional)

Create `sdkconfig.local.defaults` (git-ignored):

```
CONFIG_GROK_BUDDY_OWNER_NAME="Sam"
CONFIG_GROK_BUDDY_BOT_NAME="Bob"
CONFIG_GROK_BUDDY_TZ="PST8PDT,M3.2.0,M11.1.0"
```

The time zone (POSIX format) is used for alarms; the clock comes from NTP.
Shared setting `xai_voice` picks a voice (default `eve`; also `ara`, `rex`,
`leo`, `sal`).

### 4. Wake word (optional)

Train "Hey Grok" and save it as `wakeword/wake_word.tflite` — see
[`wakeword/README.md`](wakeword/README.md).

### 5. Build and install

From the workspace root:

```sh
python mosaico.py iris system-update --project projects/grok_desk_buddy
```

Prebuilt bundles (generic, no relay or wake word) are attached to
[Releases](../../releases): install with
`python mosaico.py iris system-update --project projects/grok_desk_buddy --bundle <file>.irisfw`.

### 6. Grok Bot hand-off (optional)

1. **Create the routine.** In the Grok Bot app, ask your bot:

   > Create a routine called "Desk buddy tasks" with a webhook trigger and no
   > schedule. On each run: the webhook body is JSON from my ESP-Mosaico desk
   > buddy with `task`, `request_id` and `reply`. Do the task as you normally
   > would for me, using your tools and memory. When finished, send one HTTP
   > POST to `reply.url` with exactly the headers in `reply.headers` and the JSON
   > body `{"request_id": <request_id>, "spoken_text": <your answer>, "status":
   > "done"}`. `spoken_text` is read aloud by a small speaker, so use 1–3 short,
   > plain spoken sentences with no markdown, lists, URLs or emoji. If you can't
   > do the task, still POST with `"status": "error"` and a one-sentence spoken
   > explanation. If something needs my approval, ask me in this chat and POST a
   > short note saying you need my OK in the app. Never post anywhere except
   > `reply.url`, and never share the token from those headers.

2. **Connect it.** Open the routine on the desktop app, run
   `zsh relay/set_webhook.sh` and paste its "POST to" URL and key.

3. **"Needs you" pings (optional).** Tell your bot: *"Remember the desk buddy's
   reply URL and headers. Whenever you're waiting on me — a card to open, an
   approval, a choice — POST a one-sentence spoken note there with
   `"status": "attention"`."* The buddy double-chimes and reads it.

## How it's built

| Path | Role |
| --- | --- |
| `components/app_buddy` | Eyes, moods, reactions, sleep, alarm face (portable to the GSP PC simulator) |
| `components/app_voice` | Wi-Fi, realtime voice session, audio pipeline, tools, Grok Bot relay client |
| `components/app_wake` | "Hey Grok" detection: TFLite Micro + the microWakeWord audio frontend |
| `components/app_motion` | BMI270 gestures (shake, bump, tilt, face-down) and haptics |
| `components/app_alarm` | NTP clock, alarms in NVS, ringing |
| `components/app_config` | Reads Vibe Mode shared settings |
| `relay/` | Cloudflare Worker + Durable Object: voice proxy with budget, Grok Bot tasks/results |
| `wakeword/` | Wake-word training scripts |

Run the face in the simulator with
`python mosaico.py project sim --project projects/grok_desk_buddy --interactive`.
CI builds every push ([`.github/workflows/build.yml`](.github/workflows/build.yml)).

Secrets never live in the repo: `secrets/`, `.gateway/` (installer archives
containing built firmware), `sdkconfig.local.defaults` and wake-word models are
git-ignored.

## License

Apache-2.0 (see [LICENSE](LICENSE)). Vendored: TensorFlow Lite Micro audio
frontend (Apache-2.0) and kissfft (BSD-3-Clause, notice in its sources) in
`components/app_wake/frontend`; DejaVu fonts under their own license in
`components/app_buddy/ui/fonts/LICENSE`.
