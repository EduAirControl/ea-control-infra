# Jenkins LTS con docker CLI + compose plugin (usa el sock del host), jq/curl
# para los smoke tests y git. JDK 17 incluido en la imagen base (los servicios
# usan el wrapper de Maven, no hace falta instalar Maven).
#
# NOTA: en Debian 13 el paquete `docker.io` del repo base solo trae el daemon
# (dockerd) — el CLI va en docker-ce-cli del repo oficial de Docker.
FROM jenkins/jenkins:lts-jdk17

USER root
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl jq gnupg \
 && install -m 0755 -d /etc/apt/keyrings \
 && curl -fsSL https://download.docker.com/linux/debian/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg \
 && chmod a+r /etc/apt/keyrings/docker.gpg \
 && echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/debian $(. /etc/os-release && echo $VERSION_CODENAME) stable" \
      > /etc/apt/sources.list.d/docker.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends docker-ce-cli docker-compose-plugin \
 && rm -rf /var/lib/apt/lists/*

# Node 22 LTS — los jobs test-web ejecutan `npm ci` + `npm run build` de
# apps/web (el build de la imagen del deploy si usa el Dockerfile de la app,
# que ya trae su propio node en la etapa builder).
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - \
 && apt-get install -y --no-install-recommends nodejs \
 && rm -rf /var/lib/apt/lists/*

USER jenkins
