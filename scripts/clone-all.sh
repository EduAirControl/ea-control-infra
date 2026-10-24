#!/usr/bin/env bash
# Clona los repositorios hermanos de infra/ con los nombres que espera
# docker-compose.yml (los nombres reales de la organizacion EduAirControl).
#
# Uso:
#   ./scripts/clone-all.sh
#   ORG=mi-org PROTO=ssh ./scripts/clone-all.sh
#
# Los clona como hermanos de infra/ (el compose construye con build: ../<repo>).
set -euo pipefail

ORG="${ORG:-EduAirControl}"
PROTO="${PROTO:-https}"
TARGET="$(cd "$(dirname "$0")/../.." && pwd)"

repos=(
  ea-control-api-gateway
  ea-control-ms-security
  ea-control-ms-classroom-management
  ea-control-ms-sensor-management
  ea-control-ms-user-management
  ea-control-ms-user-experience
  ms-environment-monitoring
  ea-control-frontend
)

for repo in "${repos[@]}"; do
  if [ -d "$TARGET/$repo/.git" ]; then
    echo "ya existe: $repo (se omite)"
    continue
  fi
  echo "clonando $repo ..."
  git clone "$PROTO://github.com/$ORG/$repo.git" "$TARGET/$repo"
done

echo
echo "Listo. Siguiente paso: cd infra && ./up.sh"
