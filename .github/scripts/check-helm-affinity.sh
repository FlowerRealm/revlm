#!/bin/sh
set -eu

HELM="${HELM:-helm}"
tmp_dir="${TMPDIR:-/tmp}/tmp-revlm-helm-affinity-$$"
mkdir -p "$tmp_dir"
trap 'rm -rf "$tmp_dir"' EXIT

render() {
  name="$1"
  shift
  "$HELM" template tmp-revlm charts/revlm "$@" > "$tmp_dir/$name.yaml"
}

assert_has() {
  file="$1"
  pattern="$2"
  if ! grep -q "$pattern" "$tmp_dir/$file.yaml"; then
    echo "expected $file to contain $pattern" >&2
    exit 1
  fi
}

assert_not_has() {
  file="$1"
  pattern="$2"
  if grep -q "$pattern" "$tmp_dir/$file.yaml"; then
    echo "expected $file not to contain $pattern" >&2
    exit 1
  fi
}

render default
assert_has default "podAntiAffinity"

render multi --set components.api.replicas=3
assert_has multi "podAntiAffinity"

cat > "$tmp_dir/custom-affinity.yaml" <<'YAML'
components:
  api:
    affinity:
      nodeAffinity:
        requiredDuringSchedulingIgnoredDuringExecution:
          nodeSelectorTerms:
            - matchExpressions:
                - key: kubernetes.io/os
                  operator: In
                  values:
                    - linux
YAML
render custom -f "$tmp_dir/custom-affinity.yaml"
assert_has custom "nodeAffinity"
assert_not_has custom "podAntiAffinity"

cat > "$tmp_dir/clear-affinity.yaml" <<'YAML'
components:
  api:
    affinity: {}
YAML
render clear -f "$tmp_dir/clear-affinity.yaml"
assert_not_has clear "podAntiAffinity"

echo "helm affinity checks passed"
