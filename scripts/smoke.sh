#!/usr/bin/env bash
# Smoke test end-to-end a través del gateway. Requiere el stack levantado
# (docker compose up -d) y jq instalado.
#
# The only bootstrap seed is the SUPER_ADMIN account (SuperAdminBootstrap). The
# smoke uses it to create a temporary institution, register a user with its
# companyCode and validate login/logout/revocation.
# Credentials can be overridden with SUPERADMIN_EMAIL / SUPERADMIN_PASSWORD.
set -euo pipefail

GATEWAY="${GATEWAY_URL:-http://localhost:8080}"
SUPERADMIN_EMAIL="${SUPERADMIN_EMAIL:-admin@eduaircontrol.com}"
# Obligatorio (R-002): la password por defecto documentada en el repo dejo de existir.
SUPERADMIN_PASSWORD="${SUPERADMIN_PASSWORD:?Set SUPERADMIN_PASSWORD (infra/.env) before running smoke}"
SUFFIX="$(date +%s)"
EMAIL="smoke-${SUFFIX}@example.com"
CONFIG_CODE="SMK-$(printf '%04d' $((SUFFIX % 10000)))"

echo "== health del gateway =="
curl -fsS "$GATEWAY/health" | jq .

echo "== SUPER_ADMIN login =="
SA_LOGIN=$(curl -fsS -X POST "$GATEWAY/api/v1/auth/login" -H 'Content-Type: application/json' \
  -d "{\"email\":\"$SUPERADMIN_EMAIL\",\"password\":\"$SUPERADMIN_PASSWORD\"}")
SA_TOKEN=$(echo "$SA_LOGIN" | jq -r .accessToken)
echo "admin token obtained: $([ -n "$SA_TOKEN" ] && echo yes || echo no)"

echo "== create test institution (code $CONFIG_CODE) =="
curl -fsS -X POST "$GATEWAY/api/v1/institutions" -H "Authorization: Bearer $SA_TOKEN" \
  -H 'Content-Type: application/json' \
  -d "{\"code\":\"$CONFIG_CODE\",\"name\":\"Smoke Institution ${SUFFIX}\",\"type\":\"education\"}" \
  | jq '{code: .code, name: .name, status: .status}'

echo "== registro =="
REG=$(curl -fsS -X POST "$GATEWAY/api/v1/auth/register" -H 'Content-Type: application/json' \
  -d "{\"email\":\"$EMAIL\",\"password\":\"SecurePass123!\",\"username\":\"smoke${SUFFIX}\",\"companyCode\":\"$CONFIG_CODE\"}")
echo "$REG" | jq '{email: .user.email, roles: .user.roles}'

echo "== login =="
LOGIN=$(curl -fsS -X POST "$GATEWAY/api/v1/auth/login" -H 'Content-Type: application/json' \
  -d "{\"email\":\"$EMAIL\",\"password\":\"SecurePass123!\"}")
TOKEN=$(echo "$LOGIN" | jq -r .accessToken)
REFRESH=$(echo "$LOGIN" | jq -r .refreshToken)
echo "access token obtenido: $([ -n "$TOKEN" ] && echo si || echo no)"

echo "== logout (revoca token) =="
curl -fsS -o /dev/null -w 'logout http=%{http_code}\n' -X POST "$GATEWAY/api/v1/auth/logout" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"refreshToken\":\"$REFRESH\",\"allDevices\":false}"

echo "== reutilizar token revocado (debe dar 401 TOKEN_REVOKED) =="
curl -sS -o /dev/null -w 'reuse http=%{http_code}\n' -X POST "$GATEWAY/api/v1/auth/logout" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{}'

# --- Flujo de negocio end-to-end: ambiente -> sensor -> medicion -> dashboard ---
# Usa el SUPER_ADMIN para el alta (escrituras ADMIN) y una credencial DEVICE para la
# ingesta. Los UUID de catalogo son los sembrados por classroom/sensor/monitoring.
ENVIRONMENT_ID="00000000-0000-4000-8000-000000000401"   # Aula 101 (seed classroom)
SENSOR_MODEL_ID="00000000-0000-4000-8000-000000000204"  # ESP32_FULL (seed sensor)
SENSOR_STATUS_ID="00000000-0000-4000-8000-000000000211" # ACTIVE (seed sensor)
TEMPERATURE_VARIABLE_ID="00000000-0000-4000-8000-000000000071"
NOW="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

echo "== alta de sensor =="
SENSOR=$(curl -fsS -X POST "$GATEWAY/api/v1/sensors" -H "Authorization: Bearer $SA_TOKEN" \
  -H 'Content-Type: application/json' \
  -d "{\"serialNumber\":\"SMK-$SUFFIX\",\"sensorModelId\":\"$SENSOR_MODEL_ID\",\"sensorStatusId\":\"$SENSOR_STATUS_ID\"}")
SENSOR_ID=$(echo "$SENSOR" | jq -r .sensorId)
echo "sensor: $SENSOR_ID"

echo "== instalacion del sensor en el ambiente $ENVIRONMENT_ID =="
INSTALLATION=$(curl -fsS -X POST "$GATEWAY/api/v1/sensor-installations" \
  -H "Authorization: Bearer $SA_TOKEN" -H 'Content-Type: application/json' \
  -d "{\"sensorId\":\"$SENSOR_ID\",\"educationalEnvironmentId\":\"$ENVIRONMENT_ID\",\"installedAt\":\"$NOW\"}")
INSTALLATION_ID=$(echo "$INSTALLATION" | jq -r .sensorInstallationId)
echo "instalacion: $INSTALLATION_ID"

echo "== credencial de dispositivo (token DEVICE del sensor) =="
DEVICE=$(curl -fsS -X POST "$GATEWAY/api/v1/device-tokens" -H "Authorization: Bearer $SA_TOKEN" \
  -H 'Content-Type: application/json' -d "{\"subject\":\"$SENSOR_ID\"}")
DEVICE_TOKEN=$(echo "$DEVICE" | jq -r .accessToken)
echo "device token: $([ -n "$DEVICE_TOKEN" ] && echo si || echo no)"

# La instalacion llega a monitoring por el broker (outbox -> RabbitMQ). Se reintenta
# la ingesta hasta que la proyeccion este lista: si no, el ingest responde 410.
echo "== ingesta de medicion (espera la proyeccion del broker) =="
ingest_http="000"
for attempt in $(seq 1 30); do
  ingest_http=$(curl -sS -o /tmp/smoke-measurement.json -w '%{http_code}' \
    -X POST "$GATEWAY/api/v1/measurements" -H "Authorization: Bearer $DEVICE_TOKEN" \
    -H 'Content-Type: application/json' \
    -d "{\"sensorInstallationId\":\"$INSTALLATION_ID\",\"variableId\":\"$TEMPERATURE_VARIABLE_ID\",\"value\":23.5,\"measuredAt\":\"$NOW\"}")
  [ "$ingest_http" = "201" ] && break
  sleep 2
done
if [ "$ingest_http" != "201" ]; then
  echo "la ingesta fallo (http=$ingest_http): $(cat /tmp/smoke-measurement.json 2>/dev/null)"
  exit 1
fi
echo "medicion ingerida: $(cat /tmp/smoke-measurement.json)"

echo "== dashboard /summary refleja el dato recien ingerido =="
SUMMARY=$(curl -fsS "$GATEWAY/api/v1/dashboard/summary" -H "Authorization: Bearer $SA_TOKEN")
echo "$SUMMARY" | jq '{counts: .counts, lastUpdated: .lastUpdated}'
MEASUREMENTS=$(echo "$SUMMARY" | jq -r .counts.measurements)
if [ "${MEASUREMENTS:-0}" -lt 1 ]; then
  echo "el dashboard no ve la medicion (measurements=$MEASUREMENTS)"
  exit 1
fi
echo "el dashboard ve $MEASUREMENTS medicion(es)"

echo "Smoke completado."