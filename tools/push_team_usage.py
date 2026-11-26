# Push Grok team usage to the relay for the desk buddy's usage screen.
# Runs inside browser-harness (needs the owner's logged-in Chrome):
#   browser-harness < tools/push_team_usage.py
# Reads grok.com dashboard usage with Chrome's own grok.com cookies (never stored
# or sent anywhere but grok.com) and posts only the numbers to the relay.
import json, os, urllib.request, datetime

SECRETS = os.path.join(os.path.expanduser("~/Hardware/esp-mosaico/esp-mosaico-vibe/projects/grok_desk_buddy"), "secrets")
read = lambda n: open(os.path.join(SECRETS, n)).read().strip()

cookies = cdp("Storage.getCookies")["cookies"]
jar = "; ".join(f'{c["name"]}={c["value"]}' for c in cookies if c["domain"].lstrip(".").endswith("grok.com"))
ua = cdp("Browser.getVersion")["userAgent"]

def dash(path, post=False):
    req = urllib.request.Request("https://grok.com/dashboard/api/dashboard/" + path, data=b"{}" if post else None,
        headers={"cookie": jar, "user-agent": ua, "content-type": "application/json",
                 "origin": "https://grok.com", "referer": "https://grok.com/dashboard/usage"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read())

status = dash("v2/usage-status?scope=team")
period = dash("get-current-period-usage", post=True)
jar = None
m = status.get("monthlyUsage") or {}
plan = period.get("planUsage") or {}
end_ms = period.get("billingCycleEnd")
summary = {
    "month_used_usd": (m.get("usedCents") or 0) / 100,
    "month_limit_usd": (m.get("limitCents") or 0) / 100,
    "month_resets": m.get("resetsAt"),
    "week_pct": plan.get("totalPercentUsed"),
    "week_resets": datetime.datetime.fromtimestamp(int(end_ms) / 1000, datetime.timezone.utc).isoformat()[:19] + "Z" if end_ms else None,
}
req = urllib.request.Request(read("relay_url").rstrip("/") + "/team-usage", data=json.dumps(summary).encode(),
    headers={"authorization": "Bearer " + read("usage_token"), "content-type": "application/json",
             "user-agent": "grok-usage-helper/1"})
with urllib.request.urlopen(req, timeout=20) as r:
    print(datetime.datetime.now().isoformat(timespec="seconds"), r.status, json.dumps(summary))
