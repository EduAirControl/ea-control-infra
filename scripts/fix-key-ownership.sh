#!/usr/bin/env bash
# Ajusta el propietario de infra/secrets/*.pem para que el usuario con el que
# corre ms-security en el contenedor pueda leerlos (el compose los monta ro).
#
# Desde el commit 6605ca8 (H-19) la imagen de ms-security corre como `appuser`
# (no-root). Un fichero 600 root:root en el host NO es legible desde el
# contenedor y JwtService aborta al arrancar con "Invalid RSA private key at
# /app/keys/test-private.pem" -> crash-loop -> el gateway responde 503 en
# /health (incidente 2026-10-26).
#
# El uid NO se asume: se detecta del contenedor o de la imagen real, de modo
# que sigue funcionando si cambia el usuario de la imagen. Si no hay docker o
# no se puede detectar, se emite un aviso con el comando exacto a ejecutar y el
# script NO falla (no debe bloquear un levantamiento).
#
# Uso: ./scripts/fix-key-ownership.sh       (idempotente)
set -euo pipefail
cd "$(dirname "$0")/.."

KEY_DIR="${SECRETS_DIR:-secrets}"
KEYS=("$KEY_DIR/test-private.pem" "$KEY_DIR/test-public.pem")

warn() { echo "fix-key-ownership: AVISO - $*" >&2; }

# Imagen de ms-security que usa el stack. Se resuelve con la misma sobrecarga de
# .env que el pipeline, porque REGISTRY/TAG pueden cambiarla.
detect_image() {
  local img=""
  if [ -f .env ]; then
    set -a
    # shellcheck disable=SC1091
    . ./.env 2>/dev/null || true
    set +a
  fi
  img="$(docker compose config --images 2>/dev/null | grep -m1 ms-security || true)"
  if [ -z "$img" ]; then
    img="$(docker image ls --format '{{.Repository}}:{{.Tag}}' 2>/dev/null \
           | grep -m1 ms-security || true)"
  fi
  printf '%s' "$img"
}

# Imprime "<uid> <gid>" del usuario con el que corre ms-security. Devuelve 1 si
# no se puede detectar (sin docker o sin imagen disponible).
detect_user() {
  local img user uid gid
  img="$(detect_image)"
  user=""

  # 1) contenedor ya creado: el valor real con el que va a arrancar.
  if docker ps -a --format '{{.Names}}' 2>/dev/null | grep -qx ms-security; then
    user="$(docker inspect ms-security --format '{{.Config.User}}' 2>/dev/null || true)"
  fi
  # 2) usuario declarado en la imagen (aun sin contenedor).
  if [ -z "$user" ] && [ -n "$img" ]; then
    user="$(docker image inspect "$img" --format '{{.Config.User}}' 2>/dev/null || true)"
  fi
  # Sin USER declarado el contenedor corre como root.
  [ -n "$user" ] || user=0

  uid="${user%%:*}"
  gid="${user#*:}"
  [ "$gid" = "$user" ] && gid=""

  # Un nombre de usuario hay que resolverlo dentro de la imagen.
  case "$uid" in
    *[!0-9]*)
      [ -n "$img" ] || return 1
      uid="$(docker run --rm --entrypoint id "$img" -u "$uid" 2>/dev/null)" || return 1 ;;
  esac
  case "$gid" in
    ""|*[!0-9]*) gid="$uid" ;;
  esac

  echo "$uid $gid"
}

# Sin claves todavia: nada que arreglar (gen-dev-keys.sh las creara).
for f in "${KEYS[@]}"; do
  if [ ! -f "$f" ]; then
    echo "fix-key-ownership: falta $f (nada que hacer)"
    exit 0
  fi
done

if ! command -v docker >/dev/null 2>&1; then
  warn "docker no esta disponible: no se pudo detectar el usuario de ms-security."
  warn "si ms-security aborta con 'Invalid RSA private key', ejecuta en el host:"
  warn "  sudo docker run --rm --entrypoint id <imagen-ms-security> -u   # ver el uid"
  warn "  sudo chown <uid>:<gid> secrets/test-*.pem"
  exit 0
fi

if ! uid_gid="$(detect_user)"; then
  warn "no se pudo detectar el usuario con el que corre ms-security (sin imagen)."
  warn "si ms-security aborta con 'Invalid RSA private key', ejecuta en el host:"
  warn "  sudo chown <uid>:<gid> secrets/test-*.pem"
  exit 0
fi
uid="${uid_gid% *}"
gid="${uid_gid#* }"

current="$(stat -c '%u:%g' "${KEYS[0]}")"
if [ "$current" = "$uid:$gid" ]; then
  echo "fix-key-ownership: ok (propietario de secrets/*.pem ya es $uid:$gid)"
else
  echo "fix-key-ownership: propietario de secrets/*.pem $current -> $uid:$gid"
  if [ "$(id -u)" = 0 ]; then
    chown "$uid:$gid" "${KEYS[@]}" || { warn "chown falló ($?)."; exit 0; }
  elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    sudo chown "$uid:$gid" "${KEYS[@]}" || { warn "sudo chown falló ($?)."; exit 0; }
  elif command -v setfacl >/dev/null 2>&1; then
    # Sin privilegios para chown: basta con dar permiso de lectura al uid. El
    # fichero sigue en 600, no se abre a todo el mundo.
    for f in "${KEYS[@]}"; do
      setfacl -m "u:$uid:r" "$f" || { warn "setfacl falló en $f ($?)."; exit 0; }
    done
    echo "fix-key-ownership: permiso de lectura concedido via ACL (sin cambiar el dueno)"
  else
    warn "no hay forma de dar permiso de lectura al uid $uid sin privilegios."
    warn "ejecuta como root:  sudo chown $uid:$gid secrets/test-*.pem"
    exit 0
  fi
fi

# Comprobacion empirica con la imagen real: el uid debe poder leer los ficheros.
img="$(detect_image)"
if [ -n "$img" ] && docker image inspect "$img" >/dev/null 2>&1; then
  key_dir="$(cd "$KEY_DIR" && pwd)"
  if docker run --rm --user "$uid:$gid" \
       -v "$key_dir/test-private.pem:/chk/test-private.pem:ro" \
       -v "$key_dir/test-public.pem:/chk/test-public.pem:ro" \
       --entrypoint sh "$img" -c 'cat /chk/test-private.pem /chk/test-public.pem >/dev/null' \
       >/dev/null 2>&1; then
    echo "fix-key-ownership: verificado (lectura como uid $uid sobre $img)"
  else
    warn "verificacion FALLIDA: el uid $uid no puede leer secrets/test-*.pem."
    warn "revisa permisos con: ls -la secrets/"
  fi
fi
