#!/usr/bin/env bash
# Para el stack de EduAirControl.
#
# Uso:
#   ./down.sh                # para y quita contenedores (los datos quedan)
#   ./down.sh --volumes      # ademas borra las bases de datos
#   ./down.sh --images       # ademas borra las imagenes construidas
set -euo pipefail

cd "$(dirname "$0")"

ARGS=()
for arg in "$@"; do
  case "$arg" in
    --volumes) ARGS+=(--volumes) ;;
    --images)  ARGS+=(--rmi local) ;;
    -h|--help)
      sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "opcion desconocida: $arg" >&2; exit 2 ;;
  esac
done

if [ "${1:-}" = "--volumes" ]; then
  echo "ATENCION: se van a borrar las bases de datos del stack."
  read -r -p "Escribe 'borrar' para confirmar: " answer
  [ "$answer" = "borrar" ] || { echo "cancelado."; exit 1; }
fi

docker compose down --remove-orphans "${ARGS[@]}"
echo "Stack detenido."
