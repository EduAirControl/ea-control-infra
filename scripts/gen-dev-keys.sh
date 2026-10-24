#!/usr/bin/env bash
# Genera el par de claves RSA de DESARROLLO para ms-security en ./secrets.
#
# El stack local (docker-compose.yml) monta ./secrets/test-private.pem y
# ./secrets/test-public.pem en ms-security. Estas claves son solo para local:
# NUNCA se suben (secrets/ esta en .gitignore) y NUNCA deben usarse en produccion.
#
# Uso: ./scripts/gen-dev-keys.sh
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p secrets

# Limpia cualquier resto que no sea un fichero (p. ej. directorios creados por un
# montaje de Docker previo) para que openssl pueda escribir la clave.
for f in secrets/test-private.pem secrets/test-public.pem; do
  if [ -e "$f" ] && [ ! -f "$f" ]; then
    echo "quitando resto no-fichero: $f"
    if ! rm -rf "$f" 2>/dev/null; then
      echo "no se pudo borrar '$f' (probablemente lo creo Docker como root)." >&2
      echo "borralo con: sudo rm -rf '$f'  y vuelve a ejecutar este script." >&2
      exit 1
    fi
  fi
done

if [ -s secrets/test-private.pem ] && [ -s secrets/test-public.pem ]; then
  echo "las claves de desarrollo ya existen (se omiten)"
  exit 0
fi

openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out secrets/test-private.pem
openssl pkey -in secrets/test-private.pem -pubout -out secrets/test-public.pem
echo "claves generadas: infra/secrets/test-private.pem y test-public.pem"
