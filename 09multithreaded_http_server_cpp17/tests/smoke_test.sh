#!/usr/bin/env bash
set -euo pipefail
BASE_URL="${1:-http://127.0.0.1:8080}"

echo "Testing GET /"
curl --fail --silent --show-error "$BASE_URL/" | grep -Fq "Hello from the multithreaded HTTP server!"
echo "Testing GET /health"
test "$(curl --fail --silent --show-error "$BASE_URL/health")" = "OK"
echo "Testing GET /info"
curl --fail --silent --show-error "$BASE_URL/info" | grep -Fq "work-stealing thread pool"
echo "Testing 404"
test "$(curl --silent -o /dev/null -w '%{http_code}' "$BASE_URL/not-found")" = "404"
echo "Testing 405"
test "$(curl --silent -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/")" = "405"
echo "All smoke tests passed."
