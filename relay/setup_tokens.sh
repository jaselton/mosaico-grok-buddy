#!/bin/zsh
# Generate the relay's device and bot tokens, store them as Worker secrets and
# save the device's copy (plus the relay URL) in ../secrets/ for the firmware.
# Usage: zsh relay/setup_tokens.sh https://grok-buddy-relay.<your-subdomain>.workers.dev
set -eu
cd "${0:A:h}"
url="${1:-}"
if [[ "$url" != https://* ]]; then
  echo "Usage: zsh setup_tokens.sh https://grok-buddy-relay.<subdomain>.workers.dev"
  exit 1
fi
secrets="../secrets"
mkdir -p "$secrets"
chmod 700 "$secrets"
umask 077
device=$(openssl rand -hex 32)
bot=$(openssl rand -hex 32)
printf '%s' "$device" | wrangler secret put DEVICE_TOKEN >/dev/null
printf '%s' "$bot" | wrangler secret put BOT_TOKEN >/dev/null
printf '%s' "$device" > "$secrets/relay_token"
printf '%s' "${url%/}" > "$secrets/relay_url"
unset device bot
echo "Tokens set on the Worker; device copy saved to secrets/relay_token and secrets/relay_url."
