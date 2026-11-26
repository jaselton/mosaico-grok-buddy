#!/usr/bin/env python3
"""Speaker test for the on-device wake word (macOS).

Plays the wake phrase through a Mac speaker in several system voices and
speeds, then look-alike and ordinary phrases, while following the buddy's
log. The board runs in test mode (detections are logged, no conversation
starts, nothing is billed) at a deliberately low cutoff so every peak is
scored; the summary then suggests a cutoff that separates hits from false
triggers.

Run from the esp-mosaico-vibe workspace root (Python 3.10+), board connected and idle:
  python3.11 projects/grok_desk_buddy/wakeword/speaker_test.py [--device "Studio Display Speakers"]
"""
import argparse
import random
import re
import subprocess
import sys
import threading
import time

PROJECT = "projects/grok_desk_buddy"
FIRE_AT = 0.90   # replaced by --cutoff
POSITIVES = ["Hey Grok", "Hey, Grok.", "Hey Grok!"]
NEGATIVES = ["Hey Greg", "Hey Google", "OK Grok", "Hey Brock", "Hey Rock", "Grok",
             "Hey group", "Great job", "Hey, good morning",
             "The weather is nice today and I might go for a walk.",
             "Can you hand me the remote control please?",
             "I think the meeting moved to three o'clock."]
VOICES = ["Samantha", "Daniel", "Karen", "Moira", "Rishi", "Fred",
          "Eddy (English (US))", "Flo (English (US))", "Reed (English (US))", "Sandy (English (US))"]
RATES = [150, 185]
SCORE = re.compile(r"app_wake: (wake word|near miss|peak) \((?:score|peak) ([0-9.]+)")


def rpc(method, payload=""):
    """Call the board's wake-word tuning service; returns its reply text."""
    import base64
    import json
    out = subprocess.run([sys.executable, "mosaico.py", "--json", "iris", "rpc", "18263", str(method),
                          "--payload", payload, "--project", PROJECT],
                         capture_output=True, text=True, timeout=30)
    try:
        reply = json.loads(out.stdout.strip().splitlines()[-1])
        return base64.b64decode(reply["response"]["payload_base64"]).decode()
    except Exception:
        return (out.stdout + out.stderr).strip()[-200:]


class LogFollower(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.events = []   # (time, kind, score)
        self.proc = subprocess.Popen(
            [sys.executable, "mosaico.py", "iris", "logs", "--timeout", "3600", "--disable-auto-color",
             "--project", PROJECT], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)

    def run(self):
        for line in self.proc.stdout:
            m = SCORE.search(line)
            if m:
                self.events.append((time.time(), m.group(1), float(m.group(2))))

    def since(self, t0):
        return [e for e in self.events if e[0] >= t0]


def available_voices():
    out = subprocess.run(["say", "-v", "?"], capture_output=True, text=True).stdout
    installed = {re.split(r"\s{2,}", line.strip())[0] for line in out.splitlines() if line.strip()}
    return [v for v in VOICES if v in installed]


def trial(log, text, voice, rate, device):
    t0 = time.time()
    cmd = ["say", "-v", voice, "-r", str(rate), text]
    if device:
        cmd[1:1] = ["-a", device]
    subprocess.run(cmd, check=False)
    time.sleep(2.2)   # model window + log latency
    ev = log.since(t0)
    best = max((e[2] for e in ev), default=0.0)
    # Test mode logs each event's peak; "fired" means the peak reached the
    # board's current cutoff (the real-use threshold, set below).
    fired = best >= FIRE_AT
    return fired, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="Studio Display Speakers", help="macOS output for `say -a`")
    ap.add_argument("--cutoff", type=float, default=0.90, help="threshold whose hit/false rate is shown per line")
    ap.add_argument("--negatives-per-voice", type=int, default=4)
    args = ap.parse_args()

    global FIRE_AT
    FIRE_AT = args.cutoff
    voices = available_voices()
    if not voices:
        sys.exit("none of the listed voices are installed")
    print(f"voices: {', '.join(voices)}")
    log = LogFollower()
    log.start()
    time.sleep(3)
    print("board:", rpc(2, "1"))                     # test mode: log only
    rpc(4)

    pos, neg = [], []
    try:
        for voice in voices:
            for rate in RATES:
                for text in POSITIVES:
                    fired, best = trial(log, text, voice, rate, args.device)
                    pos.append((voice, rate, text, fired, best))
                    print(f"  + {voice:28s} r{rate} {text!r:14s} {'HIT ' if fired else 'miss'} {best:.2f}")
            for text in random.sample(NEGATIVES, args.negatives_per_voice):
                fired, best = trial(log, text, voice, 170, args.device)
                neg.append((voice, text, fired, best))
                print(f"  - {voice:28s}      {text[:30]!r:32s} {'FALSE' if fired else 'ok   '} {best:.2f}")
    finally:
        print("board:", rpc(2, "0"))                 # back to normal
        log.proc.terminate()

    pos_scores = sorted(p[4] for p in pos)
    neg_scores = sorted(n[3] for n in neg)
    print("\n=== summary ===")
    for cut in (0.5, 0.6, 0.7, 0.8, 0.85, 0.9, 0.95):
        hit = sum(s >= cut for s in pos_scores) / len(pos_scores)
        false = sum(s >= cut for s in neg_scores)
        print(f"cutoff {cut:.2f}: hit rate {hit:5.1%}  false triggers {false}/{len(neg_scores)}")
    by_voice = {}
    for v, _, _, _, best in pos:
        by_voice.setdefault(v, []).append(best)
    print("weakest voices:", ", ".join(f"{v} {sum(s)/len(s):.2f}" for v, s in
                                        sorted(by_voice.items(), key=lambda kv: sum(kv[1]) / len(kv[1]))[:3]))
    worst_neg = max(neg_scores, default=0)
    viable = [c for c in (0.5, 0.6, 0.7, 0.8, 0.85, 0.9, 0.95) if c > worst_neg]
    if viable:
        best_cut = max(viable, key=lambda c: (sum(s >= c for s in pos_scores), -c))
        print(f"suggested cutoff: {best_cut:.2f} (highest negative scored {worst_neg:.2f})")
        print(f"apply with: python3 mosaico.py iris rpc 18263 1 --payload {best_cut:.2f} --project {PROJECT}")
    else:
        print("no cutoff separates hits from false triggers; retrain with more negatives")


if __name__ == "__main__":
    main()
