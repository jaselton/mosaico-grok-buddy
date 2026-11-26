#!/bin/zsh
# Save a backup Wi-Fi network (e.g. a phone hotspot) for the build-time fallback
# (secrets/wifi2_ssid + secrets/wifi2_password, git-ignored). The password is
# hidden and never echoed or logged. Rebuild/install afterwards.
set -eu
dir="${0:A:h}/../secrets"
mkdir -p "$dir"
chmod 700 "$dir"
read -r "ssid?Hotspot name (exactly as it shows in Wi-Fi lists): "
read -rs "pass?Hotspot password (input hidden), then Enter: "
echo
if [[ -z "$ssid" || ${#ssid} -gt 32 ]]; then
  echo "Name must be 1-32 characters. Nothing saved."; unset pass; exit 1
fi
if [[ -n "$pass" && ( ${#pass} -lt 8 || ${#pass} -gt 63 ) ]]; then
  echo "Password must be 8-63 characters. Nothing saved."; unset pass; exit 1
fi
umask 077
printf '%s' "$ssid" > "$dir/wifi2_ssid"
printf '%s' "$pass" > "$dir/wifi2_password"
n=${#pass}
unset pass
chmod 600 "$dir/wifi2_ssid" "$dir/wifi2_password"
echo "Saved backup network '$ssid' (password $n chars). Tell Claude it's done."
