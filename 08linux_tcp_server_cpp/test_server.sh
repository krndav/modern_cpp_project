#!/usr/bin/env bash
set -euo pipefail

base_url="${1:-http://127.0.0.1:8080}"

echo "Testing home route..."
curl --fail --silent --show-error "$base_url/" | grep -F "Hello from the Linux TCP server!"

echo "Testing health route..."
curl --fail --silent --show-error "$base_url/health" | grep -Fx "OK"

echo "Testing 404 route..."
status="$(curl --silent --output /dev/null --write-out '%{http_code}' "$base_url/missing")"
test "$status" = "404"
echo "404 route returned $status"

echo "All smoke tests passed."
