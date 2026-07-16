{{/*
Component partials. Each takes:
  dict "name" <componentName> "ctx" <root> "merged" <merged-config-dict>

The caller (components.yaml) does the defaults+component merge once per component
and passes the resulting dict in. Partials never re-merge.
*/}}

{{- define "revlm.component.serviceaccount" -}}
{{- $name := .name -}}
{{- $ctx := .ctx -}}
{{- $merged := .merged -}}
{{- $sa := default (dict) $merged.serviceAccount -}}
{{- $saName := default (include "revlm.componentFullname" (dict "name" $name "ctx" $ctx)) $sa.name -}}
{{- if .renderServiceAccount }}
apiVersion: v1
kind: ServiceAccount
metadata:
  name: {{ $saName }}
  labels:
    {{- include "revlm.componentLabels" (dict "name" $name "ctx" $ctx) | nindent 4 }}
  {{- with $sa.annotations }}
  annotations:
    {{- toYaml . | nindent 4 }}
  {{- end }}
{{- end -}}
{{- end -}}

{{- define "revlm.component.deployment" -}}
{{- $name := .name -}}
{{- $ctx := .ctx -}}
{{- $merged := .merged -}}
{{- $fullname := include "revlm.componentFullname" (dict "name" $name "ctx" $ctx) -}}
{{- $replicas := int $merged.replicas -}}
{{- if $merged.autoscaling.enabled -}}
{{- $replicas = int $merged.autoscaling.minReplicas -}}
{{- end -}}
{{- $secretName := include "revlm.secretName" $ctx -}}
{{- $sa := default (dict) $merged.serviceAccount -}}
{{- $saName := "" -}}
{{- if $sa.name -}}
{{- $saName = $sa.name -}}
{{- else if $sa.create -}}
{{- $saName = $fullname -}}
{{- end -}}
{{- $hasTmpMount := false -}}
{{- range $merged.extraVolumeMounts -}}
{{- if eq (toString .mountPath) "/tmp" -}}
{{- $hasTmpMount = true -}}
{{- end -}}
{{- end -}}
{{- $autoTmp := and $merged.securityContext.readOnlyRootFilesystem (not $hasTmpMount) -}}
{{- $tmpVolumeName := "revlm-tmp" -}}
apiVersion: apps/v1
kind: Deployment
metadata:
  name: {{ $fullname }}
  labels:
    {{- include "revlm.componentLabels" (dict "name" $name "ctx" $ctx) | nindent 4 }}
spec:
  {{- if not $merged.autoscaling.enabled }}
  replicas: {{ $merged.replicas }}
  {{- end }}
  minReadySeconds: {{ $merged.minReadySeconds }}
  strategy:
    type: RollingUpdate
    rollingUpdate:
      maxUnavailable: 0
      maxSurge: 1
  selector:
    matchLabels:
      {{- include "revlm.componentSelectorLabels" (dict "name" $name "ctx" $ctx) | nindent 6 }}
  template:
    metadata:
      labels:
        {{- include "revlm.componentSelectorLabels" (dict "name" $name "ctx" $ctx) | nindent 8 }}
        {{- with $merged.podLabels }}
        {{- toYaml . | nindent 8 }}
        {{- end }}
      {{- with $merged.podAnnotations }}
      annotations:
        {{- toYaml . | nindent 8 }}
      {{- end }}
    spec:
      {{- if $saName }}
      serviceAccountName: {{ $saName }}
      {{- end }}
      automountServiceAccountToken: false
      {{- if $merged.hostNetwork }}
      hostNetwork: true
      dnsPolicy: ClusterFirstWithHostNet
      {{- end }}
      terminationGracePeriodSeconds: {{ $merged.terminationGracePeriodSeconds }}
      {{- with $ctx.Values.imagePullSecrets }}
      imagePullSecrets:
        {{- toYaml . | nindent 8 }}
      {{- end }}
      {{- with $merged.podSecurityContext }}
      securityContext:
        {{- toYaml . | nindent 8 }}
      {{- end }}
      containers:
        - name: revlm
          image: {{ include "revlm.imageRef" $ctx | quote }}
          imagePullPolicy: {{ include "revlm.imagePullPolicy" $ctx }}
          {{- with $merged.securityContext }}
          securityContext:
            {{- toYaml . | nindent 12 }}
          {{- end }}
          ports:
            - name: http
              containerPort: {{ $merged.port }}
              protocol: TCP
          env:
            {{- include "revlm.componentEnv" (dict "name" $name "ctx" $ctx "merged" $merged) | nindent 12 }}
            {{- with $merged.extraEnv }}
            {{- toYaml . | nindent 12 }}
            {{- end }}
          {{- if or $secretName $merged.extraEnvFrom }}
          envFrom:
            {{- if $secretName }}
            - secretRef:
                name: {{ $secretName }}
            {{- end }}
            {{- with $merged.extraEnvFrom }}
            {{- toYaml . | nindent 12 }}
            {{- end }}
          {{- end }}
          livenessProbe:
            {{- toYaml $merged.probes.liveness | nindent 12 }}
          readinessProbe:
            {{- toYaml $merged.probes.readiness | nindent 12 }}
          startupProbe:
            {{- toYaml $merged.probes.startup | nindent 12 }}
          resources:
            {{- toYaml $merged.resources | nindent 12 }}
          {{- if or $autoTmp $merged.extraVolumeMounts }}
          volumeMounts:
            {{- if $autoTmp }}
            - name: {{ $tmpVolumeName }}
              mountPath: /tmp
            {{- end }}
            {{- with $merged.extraVolumeMounts }}
            {{- toYaml . | nindent 12 }}
            {{- end }}
          {{- end }}
      {{- if or $autoTmp $merged.extraVolumes }}
      volumes:
        {{- if $autoTmp }}
        - name: {{ $tmpVolumeName }}
          emptyDir: {}
        {{- end }}
        {{- with $merged.extraVolumes }}
        {{- toYaml . | nindent 8 }}
        {{- end }}
      {{- end }}
      {{- if kindIs "map" $merged.affinity }}
      {{- if $merged.affinity }}
      affinity:
        {{- toYaml $merged.affinity | nindent 8 }}
      {{- end }}
      {{- else }}
      affinity:
        podAntiAffinity:
          requiredDuringSchedulingIgnoredDuringExecution:
            - podAffinityTerm:
                topologyKey: kubernetes.io/hostname
                labelSelector:
                  matchLabels:
                    {{- include "revlm.componentSelectorLabels" (dict "name" $name "ctx" $ctx) | nindent 20 }}
      {{- end }}
      {{- if $merged.topologySpreadConstraints }}
      topologySpreadConstraints:
        {{- toYaml $merged.topologySpreadConstraints | nindent 8 }}
      {{- else if gt $replicas 1 }}
      topologySpreadConstraints:
        - maxSkew: 1
          topologyKey: kubernetes.io/hostname
          whenUnsatisfiable: DoNotSchedule
          labelSelector:
            matchLabels:
              {{- include "revlm.componentSelectorLabels" (dict "name" $name "ctx" $ctx) | nindent 14 }}
      {{- end }}
      {{- with $merged.nodeSelector }}
      nodeSelector:
        {{- toYaml . | nindent 8 }}
      {{- end }}
      {{- with $merged.tolerations }}
      tolerations:
        {{- toYaml . | nindent 8 }}
      {{- end }}
{{- end -}}

{{- define "revlm.component.service" -}}
{{- $name := .name -}}
{{- $ctx := .ctx -}}
{{- $merged := .merged -}}
{{- $svc := $merged.service -}}
{{- if $svc.enabled }}
apiVersion: v1
kind: Service
metadata:
  name: {{ include "revlm.componentFullname" (dict "name" $name "ctx" $ctx) }}
  labels:
    {{- include "revlm.componentLabels" (dict "name" $name "ctx" $ctx) | nindent 4 }}
  {{- with $svc.annotations }}
  annotations:
    {{- toYaml . | nindent 4 }}
  {{- end }}
spec:
  type: ClusterIP
  selector:
    {{- include "revlm.componentSelectorLabels" (dict "name" $name "ctx" $ctx) | nindent 4 }}
  ports:
    - name: http
      port: {{ $svc.port }}
      targetPort: http
      protocol: TCP
{{- end -}}
{{- end -}}

{{- define "revlm.component.hpa" -}}
{{- $name := .name -}}
{{- $ctx := .ctx -}}
{{- $merged := .merged -}}
{{- $hpa := $merged.autoscaling -}}
{{- if $hpa.enabled }}
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: {{ include "revlm.componentFullname" (dict "name" $name "ctx" $ctx) }}
  labels:
    {{- include "revlm.componentLabels" (dict "name" $name "ctx" $ctx) | nindent 4 }}
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: {{ include "revlm.componentFullname" (dict "name" $name "ctx" $ctx) }}
  minReplicas: {{ $hpa.minReplicas }}
  maxReplicas: {{ $hpa.maxReplicas }}
  metrics:
    {{- if $hpa.targetCPUUtilizationPercentage }}
    - type: Resource
      resource:
        name: cpu
        target:
          type: Utilization
          averageUtilization: {{ $hpa.targetCPUUtilizationPercentage }}
    {{- end }}
    {{- if $hpa.targetMemoryUtilizationPercentage }}
    - type: Resource
      resource:
        name: memory
        target:
          type: Utilization
          averageUtilization: {{ $hpa.targetMemoryUtilizationPercentage }}
    {{- end }}
    {{- with $hpa.customMetrics }}
    {{- toYaml . | nindent 4 }}
    {{- end }}
  {{- with $hpa.behavior }}
  behavior:
    {{- toYaml . | nindent 4 }}
  {{- end }}
{{- end -}}
{{- end -}}

{{- define "revlm.component.pdb" -}}
{{- $name := .name -}}
{{- $ctx := .ctx -}}
{{- $merged := .merged -}}
{{- $pdb := $merged.pdb -}}
{{- $replicas := int $merged.replicas -}}
{{- if $merged.autoscaling.enabled -}}
{{- $replicas = int $merged.autoscaling.minReplicas -}}
{{- end -}}
{{- $hasExplicit := or (not (empty $pdb.minAvailable)) (not (empty $pdb.maxUnavailable)) -}}
{{- if and $pdb.enabled (or $hasExplicit (gt $replicas 1)) }}
apiVersion: policy/v1
kind: PodDisruptionBudget
metadata:
  name: {{ include "revlm.componentFullname" (dict "name" $name "ctx" $ctx) }}
  labels:
    {{- include "revlm.componentLabels" (dict "name" $name "ctx" $ctx) | nindent 4 }}
spec:
  {{- if not (empty $pdb.minAvailable) }}
  minAvailable: {{ $pdb.minAvailable }}
  {{- else if not (empty $pdb.maxUnavailable) }}
  maxUnavailable: {{ $pdb.maxUnavailable }}
  {{- else if ge $replicas 3 }}
  minAvailable: 2
  {{- else }}
  minAvailable: 1
  {{- end }}
  selector:
    matchLabels:
      {{- include "revlm.componentSelectorLabels" (dict "name" $name "ctx" $ctx) | nindent 6 }}
{{- end -}}
{{- end -}}
