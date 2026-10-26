#!/usr/bin/env bash
# Levanta todo EduAirControl de una vez: build, arranque y espera a que responda.
#
# Uso:
#   ./up.sh                  # build + up + espera
#   ./up.sh --no-build       # reutiliza las imagenes que ya haya
#   ./down.sh                # lo para (ver down.sh)
#
# IMPORTANTE: este es el UNICO compose del proyecto. Los compose de cada
# servicio (ms-*/docker-compose.yml) y el de api-gateway/docker-compose.yml son
# para trabajar un servicio aislado y NO se pueden levantar a la vez que este:
# compiten por nombres de contenedor y por puertos.
set -euo pipefail

cd "$(dirname "$0")"

BUILD=1
for arg in "$@"; do
  case "$arg" in
    --no-build) BUILD=0 ;;
    -h|--help)
      sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "opcion desconocida: $arg" >&2; exit 2 ;;
  esac
done

if [ ! -f .env ]; then
  echo "No hay .env. Creandolo desde .env.example (revisa SUPERADMIN_PASSWORD)."
  cp .env.example .env
fi

# Claves RSA de desarrollo de ms-security (gitignored): el stack las monta desde
# ./secrets. Se generan si faltan para que un clon nuevo levante sin pasos extra.
if [ ! -s secrets/test-private.pem ] || [ ! -s secrets/test-public.pem ]; then
  echo "Generando claves RSA de desarrollo (infra/secrets)..."
  ./scripts/gen-dev-keys.sh
else
  # Ya existen: aun asi hay que asegurar que el usuario con el que corre
  # ms-security (no-root desde H-19) pueda leerlas. Ver incidente 2026-10-26.
  ./scripts/fix-key-ownership.sh
fi

# Aviso si hay contenedores de los compose por servicio sueltos.
stray=$(docker ps --format '{{.Names}}' | grep -E '^(api-gateway-redis|ms-[a-z-]+-db|ms-[a-z-]+-rabbit)$' || true)
if [ -n "$stray" ]; then
  echo "Aviso: hay contenedores de otros compose que pueden competir por puertos:"
  echo "$stray" | sed 's/^/    /'
  echo "  Para quitarlos: docker rm -f $stray"
  echo
fi

if [ "$BUILD" -eq 1 ]; then
  echo "Construyendo imagenes (puede tardar la primera vez)..."
  docker compose build
else
  echo "Reutilizando imagenes existentes."
fi

echo "Levantando el stack..."
docker compose up -d --remove-orphans

echo
./scripts/wait-for-services.sh

cat <<'EOF'

┌──────────────────────────────────────────────────────────┐
│  EduAirControl listo                                     │
├──────────────────────────────────────────────────────────┤
│  Frontend web   http://localhost:3000                    │
│  API Gateway    http://localhost:8080                    │
│  Swagger        http://localhost:8081/swagger-ui.html    │
│                 (y en cada servicio: 3002..3007)         │
│  RabbitMQ       http://localhost:15672  (guest/guest)    │
├──────────────────────────────────────────────────────────┤
│  Superadmin     admin@eduaircontrol.com                  │
│  Logs           docker compose logs -f                  │
│  Parar          ./down.sh                                │
└──────────────────────────────────────────────────────────┘
EOF
