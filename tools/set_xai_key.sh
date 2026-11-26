#!/bin/zsh
# Save an xAI API key for the build-time fallback (secrets/xai_api_key, git-ignored).
# Input is hidden and never echoed or logged. Rebuild/install afterwards.
set -eu
dir="${0:A:h}/../secrets"
mkdir -p "$dir"
chmod 700 "$dir"
read -rs "key?Paste xAI API key (input hidden), then Enter: "
echo
key="${key//[[:space:]]/}"
if [[ "$key" != xai-* || ${#key} -lt 20 ]]; then
  echo "That doesn't look like an xAI key (should start with xai-). Nothing saved."
  unset key
  exit 1
fi
umask 077
printf '%s' "$key" > "$dir/xai_api_key"
unset key
chmod 600 "$dir/xai_api_key"
echo "Saved ($(wc -c < "$dir/xai_api_key" | tr -d ' ') chars) to secrets/xai_api_key. Tell Claude it's done."
