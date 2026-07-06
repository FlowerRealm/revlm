#!/usr/bin/env bash
set -euo pipefail

tag="${1:-${GITHUB_REF_NAME:-}}"
if [[ -z "$tag" ]]; then
  echo "release version check failed: missing Git tag argument or GITHUB_REF_NAME" >&2
  exit 1
fi

chart_app_version="$(
  sed -nE 's/^appVersion:[[:space:]]*"?([^"]+)"?[[:space:]]*$/\1/p' charts/revlm/Chart.yaml
)"
values_image_tag="$(
  awk '
    /^image:/ { in_image = 1; next }
    in_image && /^[^[:space:]]/ { in_image = 0 }
    in_image && /^[[:space:]]+tag:/ {
      gsub(/"/, "", $2)
      print $2
      exit
    }
  ' charts/revlm/values.yaml
)"

if [[ "$chart_app_version" != "$tag" ]]; then
  echo "release version check failed: charts/revlm/Chart.yaml appVersion=$chart_app_version, git tag=$tag" >&2
  exit 1
fi

if [[ "$values_image_tag" != "$tag" ]]; then
  echo "release version check failed: charts/revlm/values.yaml image.tag=$values_image_tag, git tag=$tag" >&2
  exit 1
fi

echo "release version check passed: $tag"
