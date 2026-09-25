#!/usr/bin/env bash
# Compare spec/openapi.json with the live schema. Exits 1 when they differ.
set -euo pipefail
cd "$(dirname "$0")/.."
live="$(mktemp)"
trap 'rm -f "$live"' EXIT
curl -fsSL https://api.typesafe.ai/openapi.json -o "$live"
if diff -u <(python3 -m json.tool --sort-keys spec/openapi.json) <(python3 -m json.tool --sort-keys "$live"); then
  echo "spec/openapi.json matches the live schema."
else
  echo "The live schema changed: review the diff, update spec/openapi.json, and check the SDK." >&2
  exit 1
fi
