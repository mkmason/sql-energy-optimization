#!/usr/bin/env bash
set -eu
# post_to_sigless.sh <remote:host:port> <CHANNEL> "<message>"
# Example:
#   ./post_to_sigless.sh 127.0.0.1:8000 CH1 "start,DK1081104,cpu"

REMOTE_ADDRESS="${1:-}"
CHANNEL="${2:-}"
MESSAGE="${3:-}"

if [ -z "$REMOTE_ADDRESS" ]; then
  echo "Usage: $0 <host:port> <CHANNEL> \"<message>\""
  exit 1
fi
if [ -z "$CHANNEL" ]; then
  echo "Please provide a channel, e.g. CH1 or CH2"
  exit 2
fi

# Build JSON safely; we escape any double-quotes in MESSAGE
esc_message=$(printf '%s' "$MESSAGE" | sed 's/"/\\"/g')
payload="{ \"message\": \"${esc_message}\", \"channelId\": \"${CHANNEL}\" }"
url="http://${REMOTE_ADDRESS}/api/log"

# Attempt to POST (fail loudly if it doesn't work)
curl --fail --silent --show-error --max-time 5 -X POST \
  -H "Content-Type: application/json" \
  -d "$payload" \
  "$url" || {
    echo "ERROR: could not POST to $url" >&2
    exit 3
  }