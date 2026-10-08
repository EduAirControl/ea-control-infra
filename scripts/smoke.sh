#!/usr/bin/env bash
# Smoke test end-to-end a través del gateway. Requiere el stack levantado
# (docker compose up -d) y jq instalado.
#
# Único seed del sistema: la cuenta SUPER_ADMIN (SuperAdminBootstrap). El smoke
# la usa para crear una institución temporal, registrar un usuario con su
# companyCode y validar login/logout/revocación.
# Credenciales override con SUPERADMIN_EMAIL / SUPERADMIN_PASSWORD.
set -euo pipefail

GATEWAY="${GATEWAY_URL:-http://localhost:8080}"
SUPERADMIN_EMAIL="${SUPERADMIN_EMAIL:-admin@eduaircontrol.com}"
SUPERADMIN_PASSWORD="${SUPERADMIN_PASSWORD:-EduAirControl1!}"
SUFFIX="$(date +%s)"
EMAIL="smoke-${SUFFIX}@example.com"
CONFIG_CODE="SMOKE${SUFFIX}"

echo "== health del gateway =="
curl -fsS "$GATEWAY/health" | jq .

echo "== login SUPER_ADMIN =="
SA_LOGIN=$(curl -fsS -X POST "$GATEWAY/api/v1/auth/login" -H 'Content-Type: application/json' \
  -d "{\"email\":\"$SUPERADMIN_EMAIL\",\"password\":\"$SUPERADMIN_PASSWORD\"}")
SA_TOKEN=$(echo "$SA_LOGIN" | jq -r .accessToken)
echo "admin token obtenido: $([ -n "$SA_TOKEN" ] && echo si || echo no)"

echo "== crear institución de prueba (código $CONFIG_CODE) =="
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

echo "Smoke completado."