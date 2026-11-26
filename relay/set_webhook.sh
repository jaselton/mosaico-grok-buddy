#!/bin/zsh
# Store your Grok Bot routine's webhook URL and key as relay Worker secrets.
# Both are read with hidden input and piped straight to wrangler; nothing is echoed or saved locally.
set -eu
cd "${0:A:h}"
read -rs "url?Paste the routine's 'POST to' URL (hidden), then Enter: "
echo
url="${url//[[:space:]]/}"
if [[ "$url" != https://* ]]; then
  echo "That doesn't look like an https URL. Nothing saved."
  exit 1
fi
read -rs "key?Paste the routine's key (hidden; the value only, not 'Bearer'), then Enter: "
echo
key="${key//[[:space:]]/}"
key="${key#Bearer}"
if [[ ${#key} -lt 8 ]]; then
  echo "That key looks too short. Nothing saved."
  exit 1
fi
printf '%s' "$url" | wrangler secret put GROKBOT_WEBHOOK_URL >/dev/null
printf '%s' "$key" | wrangler secret put GROKBOT_WEBHOOK_KEY >/dev/null
unset url key
echo "Saved both secrets to grok-buddy-relay. Tell Claude it's done."
