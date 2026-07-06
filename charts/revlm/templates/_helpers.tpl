{{/*
Revlm Helm helpers — name/labels/secret/component-config/env-derivation.
*/}}

{{- define "revlm.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "revlm.fullname" -}}
{{- if .Values.fullnameOverride -}}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name (include "revlm.name" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}

{{- define "revlm.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" -}}
{{- end -}}

{{/*
Component fullname: <release>-<chartname>-<component>
Input: dict "name" <componentName> "ctx" <root>
*/}}
{{- define "revlm.componentFullname" -}}
{{- $name := .name -}}
{{- $ctx := .ctx -}}
{{- printf "%s-%s" (include "revlm.fullname" $ctx) $name | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "revlm.commonLabels" -}}
helm.sh/chart: {{ include "revlm.chart" . }}
app.kubernetes.io/name: {{ include "revlm.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/part-of: revlm
{{- end -}}

{{/*
Component labels (full). Input: dict "name" <componentName> "ctx" <root>
*/}}
{{- define "revlm.componentLabels" -}}
{{ include "revlm.commonLabels" .ctx }}
app.kubernetes.io/component: {{ .name }}
{{- end -}}

{{/*
Component selector labels (immutable subset). Input: dict "name" <name> "ctx" <root>
*/}}
{{- define "revlm.componentSelectorLabels" -}}
app.kubernetes.io/name: {{ include "revlm.name" .ctx }}
app.kubernetes.io/instance: {{ .ctx.Release.Name }}
app.kubernetes.io/component: {{ .name }}
{{- end -}}

{{- define "revlm.secretName" -}}
{{- .Values.secret.existingSecret -}}
{{- end -}}

{{- define "revlm.imageRef" -}}
{{- $img := .Values.image -}}
{{- if $img.digest -}}
{{- printf "%s@%s" $img.repository $img.digest -}}
{{- else -}}
{{- printf "%s:%s" $img.repository (default .Chart.AppVersion $img.tag) -}}
{{- end -}}
{{- end -}}

{{- define "revlm.imagePullPolicy" -}}
{{- $img := .Values.image -}}
{{- if $img.digest -}}IfNotPresent{{- else -}}{{ $img.pullPolicy }}{{- end -}}
{{- end -}}

{{/*
Component effective config: deep-merge defaults with components[name] (component wins).
Input: dict "name" <name> "ctx" <root>
Returns: merged dict.
*/}}
{{- define "revlm.componentMerged" -}}
{{- $name := .name -}}
{{- $ctx := .ctx -}}
{{- $component := index $ctx.Values.components $name -}}
{{- if not $component -}}
{{- fail (printf "components.%s not defined" $name) -}}
{{- end -}}
{{- $defaults := deepCopy $ctx.Values.defaults -}}
{{- $component = deepCopy $component -}}
{{- $merged := mergeOverwrite $defaults $component -}}
{{- toYaml $merged -}}
{{- end -}}

{{/*
Component env list (rendered ready for `env:` block).
Input: dict "name" <name> "ctx" <root> "merged" <merged-config-dict>

Composition order (later wins for same key):
  1. .Values.env                            (cluster-wide shared env)
  2. redis/*                                (auto-injected from structured config)
  3. derived runtime env (REVLM_ADDR)
  4. merged.env                             (defaults.env + component.env, already merged via mergeOverwrite)
*/}}
{{- define "revlm.componentEnv" -}}
{{- $name := .name -}}
{{- $ctx := .ctx -}}
{{- $merged := .merged -}}
{{- if not $merged -}}
{{- $merged = fromYaml (include "revlm.componentMerged" (dict "name" $name "ctx" $ctx)) -}}
{{- end -}}
{{- $defaults := $ctx.Values.defaults -}}
{{- $port := $merged.port -}}
{{- $envMap := dict -}}

{{/* 1. shared env */}}
{{- range $k, $v := $ctx.Values.env -}}
{{- $_ := set $envMap $k (toString $v) -}}
{{- end -}}

{{/* 2. redis/app-settings auto-inject */}}
{{- $r := default (dict) $ctx.Values.redis -}}
{{- if $r.addr -}}
{{- $_ := set $envMap "REVLM_REDIS_ADDR" (toString $r.addr) -}}
{{- end -}}
{{- if $r.password -}}
{{- $_ := set $envMap "REVLM_REDIS_PASSWORD" (toString $r.password) -}}
{{- end -}}

{{/* 3. derived runtime env */}}
{{- $_ := set $envMap "REVLM_ADDR" (printf ":%v" $port) -}}
{{/* 4. merged component.env override (defaults.env + component.env already merged via mergeOverwrite) */}}
{{- range $k, $v := (default (dict) $merged.env) -}}
{{- $_ := set $envMap $k (toString $v) -}}
{{- end -}}

{{/* emit sorted */}}
{{- range $k := keys $envMap | sortAlpha -}}
- name: {{ $k }}
  value: {{ index $envMap $k | quote }}
{{ end -}}
{{- end -}}

{{/*
Validate values that would otherwise render broken Kubernetes resources.
Called once from components.yaml.
*/}}
{{- define "revlm.validate" -}}
{{- $components := .Values.components -}}
{{- $selectorLabelKeys := list "app.kubernetes.io/name" "app.kubernetes.io/instance" "app.kubernetes.io/component" -}}
{{- $defaultPodLabels := default (dict) .Values.defaults.podLabels -}}
{{- range $key := $selectorLabelKeys -}}
{{- if hasKey $defaultPodLabels $key -}}
{{- fail (printf "defaults.podLabels must not set selector label %q" $key) -}}
{{- end -}}
{{- end -}}
{{- range $name, $comp := $components -}}
{{- $merged := fromYaml (include "revlm.componentMerged" (dict "name" $name "ctx" $)) -}}
{{- $podLabels := default (dict) $comp.podLabels -}}
{{- range $key := $selectorLabelKeys -}}
{{- if hasKey $podLabels $key -}}
{{- fail (printf "components.%s.podLabels must not set selector label %q" $name $key) -}}
{{- end -}}
{{- end -}}
{{- $svc := default (dict) $merged.service -}}
{{- $svcEnabled := true -}}
{{- if hasKey $svc "enabled" -}}
{{- $svcEnabled = $svc.enabled -}}
{{- end -}}
{{- end -}}
{{- if .Values.ingress.enabled -}}
{{- $hosts := default (list) .Values.ingress.hosts -}}
{{- $routes := default (list) .Values.ingress.routes -}}
{{- if eq (len $hosts) 0 -}}
{{- fail "ingress.enabled=true requires ingress.hosts to contain at least one host" -}}
{{- end -}}
{{- if eq (len $routes) 0 -}}
{{- fail "ingress.enabled=true requires ingress.routes to contain at least one route" -}}
{{- end -}}
{{- range $i, $r := $routes -}}
{{- if not (index $components $r.component) -}}
{{- fail (printf "ingress.routes[%d].component = %q references undefined component" $i $r.component) -}}
{{- end -}}
{{- $routeMerged := fromYaml (include "revlm.componentMerged" (dict "name" $r.component "ctx" $)) -}}
{{- $routeSvc := default (dict) $routeMerged.service -}}
{{- $routeSvcEnabled := true -}}
{{- if hasKey $routeSvc "enabled" -}}
{{- $routeSvcEnabled = $routeSvc.enabled -}}
{{- end -}}
{{- if not $routeSvcEnabled -}}
{{- fail (printf "ingress.routes[%d].component = %q references component with service.enabled=false" $i $r.component) -}}
{{- end -}}
{{- end -}}
{{- end -}}
{{- end -}}
