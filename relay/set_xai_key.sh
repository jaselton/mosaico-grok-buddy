#!/bin/zsh
# Replace the relay's xAI API key (Worker secret XAI_API_KEY). Input is hidden
# and never echoed or logged; the key goes straight to Cloudflare.
set -eu
cd "${0:A:h}"
read -rs "key?Paste the xAI API key for the relay (input hidden), then Enter: "
echo
key="${key//[[:space:]]/}"
if [[ "$key" != xai-* || ${#key} -lt 20 ]]; then
  echo "That doesn't look like an xAI key (should start with xai-). Nothing changed."
  unset key; exit 1
fi
info=$(curl -s -m 10 -H "Authorization: Bearer $key" https://api.x.ai/v1/api-key || true)
if [[ "$info" == *'"team_blocked":true'* || "$info" == *'"api_key_blocked":true'* ]]; then
  echo "xAI reports this key or its team as blocked. Nothing changed."
  unset key info; exit 1
fi
printf '%s' "$key" | wrangler secret put XAI_API_KEY >/dev/null
unset key info
echo "Relay key updated. Tell Claude it's done."
