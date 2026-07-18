#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if ! command -v pre-commit >/dev/null 2>&1; then
  echo "pre-commit not found. Install with: pip install pre-commit" >&2
  exit 1
fi

if [[ ! -d frontend/node_modules ]]; then
  echo "frontend/node_modules missing; installing..."
  npm --prefix frontend ci --no-audit
fi

pre-commit install --hook-type pre-commit --hook-type pre-push
echo "Installed pre-commit + pre-push hooks."
