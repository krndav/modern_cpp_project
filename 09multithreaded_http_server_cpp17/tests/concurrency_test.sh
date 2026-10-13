#!/usr/bin/env bash
set -euo pipefail
BASE_URL="${1:-http://127.0.0.1:8080}"
COUNT="${2:-100}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "Sending $COUNT concurrent requests to $BASE_URL"
for i in $(seq 1 "$COUNT"); do
    (curl --fail --silent --show-error "$BASE_URL/health" >"$TMP_DIR/$i") &
done
wait
for i in $(seq 1 "$COUNT"); do grep -Fxq "OK" "$TMP_DIR/$i"; done
echo "All $COUNT concurrent requests passed."
