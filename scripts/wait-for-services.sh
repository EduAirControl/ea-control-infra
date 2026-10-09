#!/usr/bin/env bash
# Espera a que todo el stack responda (o falla con timeout).
#
# Uso:
#   ./scripts/wait-for-services.sh           # espera los 8 servicios
#   TIMEOUT=300 ./scripts/wait-for-services.sh
set -euo pipefail

TIMEOUT="${TIMEOUT:-240}"
BASE="${BASE_URL:-http://localhost}"

declare -A ENDPOINTS=(
  [api-gateway]="$BASE:${GATEWAY_PORT:-8080}/health"
  [ms-security]="$BASE:8081/api/v1/auth/health"
  [ms-classroom-management]="$BASE:3002/health"
  [ms-environment-monitoring]="$BASE:3003/health"
  [ms-sensor-management]="$BASE:3004/health"
  [ms-user-experience]="$BASE:3006/health"
  [ms-user-management]="$BASE:3007/health"
  [web]="$BASE:${WEB_PORT:-3000}/"
)

# El gateway se deja para el final: depende de que ms-security ya responda.
ORDER=(ms-security ms-classroom-management ms-environment-monitoring ms-sensor-management
       ms-user-experience ms-user-management api-gateway web)

echo "Esperando servicios (timeout ${TIMEOUT}s)..."
failed=0
for name in "${ORDER[@]}"; do
  url="${ENDPOINTS[$name]}"
  deadline=$((SECONDS + TIMEOUT))
  until curl -fsS -o /dev/null --max-time 3 "$url"; do
    if [ "$SECONDS" -ge "$deadline" ]; then
      echo "  x $name no respondio en $url"
      failed=1
      break
    fi
    sleep 2
  done
  [ "$failed" -eq 0 ] && echo "  ok $name"
done

if [ "$failed" -ne 0 ]; then
  echo
  echo "Al menos un servicio no arranco. Mira los logs con:"
  echo "  docker compose logs --tail 50 <servicio>"
  exit 1
fi

echo
echo "Todo el stack esta listo."
