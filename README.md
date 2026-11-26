# Grok Desk Buddy for ESP-Mosaico

Big expressive Grok Bot-style eyes on the [ESP-Mosaico](https://github.com/esp-mosaico/esp-mosaico-vibe)'s
480×480 round screen, that you can **talk to**. Tap the screen, speak, and Grok
answers through the speaker in about half a second while the eyes listen, think
and talk. Optionally, it can hand real tasks to **your own Grok Bot agent** and
read the answer aloud when it comes back.

<img src="docs/eyes.png" width="240" alt="The buddy's eyes on the Mosaico screen">

> Unofficial fan project. Not affiliated with or endorsed by xAI. The eyes are
> drawn from simple geometry (`tools/render_eyes.py`) to evoke the Grok Bot look.

## What it does

- **Eyes:** blink, glance around when idle, look at you while listening, drift
  up and away while thinking, and bob while talking. The bars under them follow
  your voice and Grok's actual speech.
- **Tap to talk:** uses xAI's realtime voice API
  (`wss://api.x.ai/v1/realtime`, `grok-voice-latest`). Grok's server-side voice
  detection ends your turn; tap while it talks to interrupt.
- **Grok Bot hand-off (optional):** say "ask Bob to…" and voice Grok calls an
  `ask_grok_bot` tool. The task goes through a tiny Cloudflare Worker
  ([`relay/`](relay/)) to a webhook-triggered routine on your Grok Bot. When the
  bot answers, the buddy chimes and reads it out. Runs take seconds to minutes.
- **Audio tuned for the tiny speaker:** 16 kHz mic uplink; 24 kHz downlink
  upsampled 2× with a windowed-sinc FIR, a 300 ms jitter buffer, a 150 Hz
  high-pass and a +6 dB presence shelf with a soft limiter.

## Cost

xAI bills voice per minute (about $0.05–0.08/min at the time of writing; check
[xAI's pricing](https://docs.x.ai/developers/models/voice-agent-api)). To keep it
cheap, the buddy only connects when you tap, closes the connection 10 s after
each reply, and stops listening after 5 s of silence. A short exchange costs a
few cents. Grok Bot hand-offs run on your Grok Bot plan; the relay caps them at
30 an hour. The Cloudflare Worker fits in the free tier.

## Setup

### 1. Workspace and Wi-Fi

1. Set up the [esp-mosaico-vibe](https://github.com/esp-mosaico/esp-mosaico-vibe)
   workspace (ESP-IDF at the SHA it pins, Python 3.10+) and verify a build.
2. Clone this repo into the workspace's `projects/` folder as `grok_desk_buddy`.
3. On the board, enter **Vibe Mode → Settings** and save your Wi-Fi. The app
   reads it from Vibe Mode's shared settings.

### 2. xAI API key

Get a key at [console.x.ai](https://console.x.ai). Either:

- add a shared setting named `xai_api_key` in **Vibe Mode → Settings → Shared
  information** (preferred; nothing is stored on your computer), or
- run `zsh tools/set_xai_key.sh`, which saves it with a hidden prompt to the
  git-ignored `secrets/xai_api_key`. It is then built into the firmware, so
  anyone holding the board could extract it.

Optional shared setting `xai_voice` picks a voice (default `eve`; others include
`ara`, `rex`, `leo`, `sal`).

### 3. Names (optional)

Create `sdkconfig.local.defaults` (git-ignored) to personalise the prompts:

```
CONFIG_GROK_BUDDY_OWNER_NAME="Sam"
CONFIG_GROK_BUDDY_BOT_NAME="Bob"
```

### 4. Build and install

From the workspace root:

```sh
python mosaico.py iris system-update --project projects/grok_desk_buddy
```

Without a key or Wi-Fi the app still runs, and taps cycle the faces.

### 5. Grok Bot hand-off (optional)

1. **Deploy the relay** (needs a Cloudflare account and `wrangler login`):

   ```sh
   cd relay && wrangler deploy
   zsh setup_tokens.sh https://grok-buddy-relay.<your-subdomain>.workers.dev
   ```

   `setup_tokens.sh` generates the device and bot tokens, stores them as Worker
   secrets, and writes the device's copy to `secrets/relay_url` and
   `secrets/relay_token` for the firmware.

2. **Create the routine.** In the Grok Bot app, ask your bot:

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

3. **Connect the routine.** Open the routine on the desktop app, then run
   `zsh relay/set_webhook.sh` and paste its "POST to" URL and key at the hidden
   prompts.

4. Rebuild and install (step 4) so the firmware picks up `secrets/relay_*`.

## How it's built

| Path | Role |
| --- | --- |
| `components/app_buddy` | Eyes controller (portable to the GSP PC simulator) and the authored scene |
| `components/app_voice` | Wi-Fi, realtime voice session, audio pipeline, Grok Bot relay client |
| `components/app_config` | Reads Vibe Mode shared settings |
| `components/app_display` | Display and touch bring-up |
| `relay/` | Cloudflare Worker + Durable Object between the buddy and your Grok Bot |
| `tools/render_eyes.py` | Regenerates the eye frames |

Run the face in the simulator with
`python mosaico.py project sim --project projects/grok_desk_buddy --interactive`.

Secrets never live in the repo: `secrets/`, `.gateway/` (installer archives
that contain built firmware) and `sdkconfig.local.defaults` are git-ignored.

## License

Apache-2.0 (see [LICENSE](LICENSE)). DejaVu fonts under their own license in
`components/app_buddy/ui/fonts/LICENSE`.
