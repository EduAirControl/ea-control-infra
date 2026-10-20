# jenkins — CI/CD de EduAirControl (ADR-019)

Explicación detallada, pieza por pieza y línea por línea, de cómo funciona el
CI/CD con Jenkins en este sistema. Documento complementario de
`docs-ea-control/10-devops/ci-cd.md` (allí están triggers, credenciales,
plugins y el arranque); aquí se abre cada fichero de `infra/jenkins/`.

## Piezas

| Fichero | Qué es |
|---------|--------|
| `../docker-compose.jenkins.yml` | Overlay que levanta el contenedor Jenkins en el VPS |
| `Dockerfile` | Imagen de Jenkins con docker CLI + compose + jq |
| `seed.Jenkinsfile` | Job `seed`: crea/actualiza los 16 jobs con Job DSL |
| `jobs.groovy` | Job DSL: mapa repo↔servicio y definición de `test-*`/`deploy-*` |
| `Jenkinsfile.test` | Pipeline de tests (8 jobs lo usan) |
| `Jenkinsfile.deploy` | Pipeline de deploy (8 jobs lo usan, solo encadenado) |

## 1. La máquina: `../docker-compose.jenkins.yml`

Overlay **opcional**: solo se usa en el VPS (junto a los overlays base y
cloud), nunca en local.

| Línea | Qué hace |
|-------|----------|
| `build: ./jenkins` | Construye la imagen desde `infra/jenkins/Dockerfile` |
| `mem_limit: 1024m` + `JAVA_OPTS -Xmx768m` | El VPS tiene 8 GB y además sirve el stack completo; se limita la memoria |
| `user: root` | Necesario para escribir en `/var/run/docker.sock` del host (MVP) |
| `TESTCONTAINERS_HOST_OVERRIDE: host.docker.internal` + `extra_hosts` | Los tests de `ms-environment-monitoring` y `ms-user-experience` usan Testcontainers; como Jenkins corre en Docker-outside-of-Docker, el host se alcanza como `host.docker.internal`, no como `localhost` |
| `127.0.0.1:${JENKINS_PORT:-8085}:8080` | El panel **solo** escucha en loopback; se accede por Caddy (`jenkins.eduaircontrol.tech`) o túnel SSH |
| `jenkins_home:/var/jenkins_home` | Volumen persistente: configuración, credenciales, historial |
| `/var/run/docker.sock` | Jenkins construye/pushea imágenes y orquesta `docker compose` usando el Docker **del host** |
| `/opt/eduaircontrol:/opt/eduaircontrol:ro` | El stack del VPS montado **solo lectura**: para leer `.env`, `secrets/` y ejecutar los scripts de smoke |

La concurrencia (=1) no se fija aquí: la gestionan los propios jobs con
`disableConcurrentBuilds`.

## 2. Imagen: `Dockerfile` (21 líneas)

- Línea 7: base `jenkins/jenkins:lts-jdk17` → JDK 17 ya incluido (los servicios
  usan `./mvnw`, no hace falta instalar Maven).
- Líneas 10-19: como `root`, instala `ca-certificates curl jq gnupg` y añade el
  repo oficial de Docker para instalar `docker-ce-cli` + `docker-compose-plugin`.
  Gotcha documentado: en Debian 13 el paquete `docker.io` del repo base solo
  trae el daemon (`dockerd`); el CLI va en `docker-ce-cli`.
- Línea 21: vuelve a `USER jenkins`.

## 3. Siembra de jobs: `seed.Jenkinsfile` + `jobs.groovy`

### `seed.Jenkinsfile` (24 líneas) — se crea **una vez a mano**

- Líneas 3-8 (comentario): cómo crearlo → *New Item > Pipeline > nombre
  `seed`*, *Pipeline script from SCM*, repo `ea-control-infra`, credencial
  `github-infra`, rama `*/main`, Script Path `jenkins/seed.Jenkinsfile`.
- Línea 12: `agent any` → corre en cualquier nodo.
- Línea 13: `timestamps()` → prefijo de hora en cada línea del log.
- Líneas 15-22, stage **`Seed jobs`**: una sola instrucción `jobDsl`:
  - `targets: 'jenkins/jobs.groovy'` → ejecuta ese fichero DSL.
  - `removedJobAction: 'DELETE'` → si borras un servicio del mapa, su job se
    elimina.
  - `removedViewAction: 'DELETE'` → idem con vistas.
  - `lookupStrategy: 'JENKINS_ROOT'` → los jobs se crean en la raíz de Jenkins.

> **Regla:** cada vez que cambias `jobs.groovy`, merge a `main` + *Build Now*
> en el job `seed`.

### `jobs.groovy` (103 líneas) — crea 16 jobs (8 `test-*` + 8 `deploy-*`)

- Líneas 20-23: constantes del repo infra (`INFRA_REPO`, credencial
  `github-infra`, rama **`main`** — los Jenkinsfiles viajan en releases).
- Líneas 25-34: mapa `services` de 8 entradas `[name, repo]`. Excepciones del
  patrón de nombres de GitHub (comentadas en líneas 6-10):
  - `ms-environment-monitoring` — sin prefijo `ea-control-`.
  - `ms-user-experience` — renombrado recientemente desde `-ms-user-experience`.
  - `ea-control-frontend` → servicio `web` (la app vive en `apps/web`).
- Línea 36: `services.each { svc -> ... }` → **itera una vez por servicio** y
  dentro crea dos jobs:

**Primer job: `pipelineJob("test-${svc.name}")`** (líneas 37-77)

- 39: conserva 30 builds. 40: sin builds concurrentes.
- 43-47: declara 3 parámetros `REPO_URL`, `COMMIT_SHA`, `REF` — mismos nombres
  que inyectará el webhook, así que también sirve para *Build with Parameters*
  manual.
- 48-61: `definition { cpsScm { ... } }` → el **pipeline no está inline**: se
  clona `ea-control-infra` con `github-infra` desde `*/main` y ejecuta el
  script en `jenkins/Jenkinsfile.test`.
- 62-76: `triggers { genericTrigger { ... } }` → plugin **Generic Webhook
  Trigger**:
  - 65-67: mapea JSON del webhook → variables: `$.repository.clone_url` →
    `REPO_URL`, `$.after` → `COMMIT_SHA`, `$.ref` → `REF`.
  - 69: `token("test-${svc.name}")` → la URL del webhook es
    `.../generic-webhook-trigger/invoke?token=test-<servicio>`; **el token
    identifica el job**.
  - 70: mensaje de causa `Push a $REF de <servicio>`.
  - 72-73: **filtro regex**: solo dispara si `$REF` es exactamente
    `refs/heads/develop` o `refs/heads/main` (anclado con `^...$` para no
    disparar con `develop-old` ni `mainline`).
  - 74: no loguea el JSON completo.

**Segundo job: `pipelineJob("deploy-${svc.name}")`** (líneas 79-102)

- Mismo patrón pero **sin trigger**: solo se lanza encadenado (o manualmente
  con *Build Now*).
- 86: parámetro `SERVICE_NAME` con valor por defecto `svc.name`.
- 99: `scriptPath('jenkins/Jenkinsfile.deploy')`.

## 4. Pipeline de tests: `Jenkinsfile.test` (65 líneas)

- **Línea 10** `agent any` → corre en el propio contenedor Jenkins.
- **Líneas 11-15** `options`:
  - `timestamps()` → log con hora.
  - `disableConcurrentBuilds(abortPrevious: true)` → dos pushes seguidos:
    **aborta** el build anterior y ejecuta el nuevo (siempre se verifica lo
    último).
  - `buildDiscarder(logRotator(numToKeepStr: '30'))` → conserva 30 ejecuciones.
- **Stage `Setup` (17-23)**: `env.SERVICE_NAME = env.JOB_NAME.replaceFirst('^test-', '')`
  → del nombre del job (`test-ms-security`) **deriva el servicio**
  (`ms-security`). Es lo que permite usar un único Jenkinsfile para los 8 jobs.
- **Stage `Checkout` (24-31)**: `checkout` Git manual:
  - `url: ${REPO_URL}` → el repo que hizo push (inyectado por webhook).
  - `branches: [[name: "${COMMIT_SHA}"]]` → **no** clona la rama, clona el
    commit exacto → el test corresponde 1:1 al push.
  - `CloneOption shallow: false, depth: 1, noTags: true` → clon completo sin
    tags.
- **Stage `Verify` (32-46)** — bifurcación por servicio:
  - `if (env.SERVICE_NAME == 'web')`: entra en `apps/web` y `npm ci`
    (instala según `package-lock.json`) + `npm run build`.
  - `else`: `chmod +x ./mvnw` + `./mvnw -B -ntp verify` → **`verify` es la
    palabra clave**: ejecuta unit + controller tests, `-B` batch mode sin
    prompts, `-ntp` sin transfer progress. Los servicios con Testcontainers
    usan el sock del host gracias a la config del compose.
- **Stage `Deploy gate (main)` (48-59)**:
  - `when { environment name: 'REF', value: 'refs/heads/main' }` → **solo** si
    el push fue a `main`. En `develop` el pipeline termina aquí (solo feedback
    de tests).
  - `build job: "deploy-${env.SERVICE_NAME}", wait: true, parameters: [...]`
    → dispara el job de deploy **esperando a que termine** (`wait: true`) y le
    pasa `REPO_URL`, `COMMIT_SHA` y `SERVICE_NAME`. Si el deploy falla, este
    build queda en rojo también.
- **`post` (61-64)**:
  - `failure` → loguea `Tests fallidos: <svc> @ <sha> (<repo>)`.
  - `fixed` → loguea que se recuperaron (build anterior rojo, este verde).

## 5. Pipeline de deploy: `Jenkinsfile.deploy` (104 líneas)

Solo importa el **artefacto** (imagen inmutable): el código ya está testeado.

- **Línea 8** `agent any`; **9-13** `options` → mismas que test pero
  `disableConcurrentBuilds()` **sin** `abortPrevious` (no abortas un deploy a
  medias) y conserva 20 builds.
- **Líneas 14-16** `environment { STACK_DIR = '/opt/eduaircontrol/infra' }`
  → directorio del stack en el VPS (montado RO en el contenedor).
- **Stage `Checkout` (18-25)** → idéntico al del pipeline de tests (mismo SHA).
- **Stage `Build image` (26-46)** — corre con `dir("${STACK_DIR}")` porque
  necesita el `.env`:
  - 30: `set -eu` → sale ante cualquier error o variable indefinida.
  - 31: `set -a; . ./.env; set +a` → **sourcea el `.env` exportando todo** →
    obtiene `REGISTRY`, `TAG`, `VITE_API_URL`.
  - 32: `IMAGE="ea-control-${SERVICE_NAME}"`.
  - 33-37: si es `web`, el contexto es `${WORKSPACE}/apps/web` y se pasa
    `--build-arg VITE_API_URL=${VITE_API_URL}` (la URL de la API queda
    embebida en el bundle en tiempo de build).
  - 38-42: si es un servicio Java, contexto `${WORKSPACE}` (raíz del repo,
    donde está el `Dockerfile`).
  - En ambos: dos etiquetas → `:${COMMIT_SHA}` (para rollback) y `:latest`.
- **Stage `Push GHCR` (47-64)**:
  - 49-51: `withCredentials([usernamePassword(credentialsId: 'ghcr-bot', ...)])`
    → inyecta `GH_USER`/`GH_TOKEN` **solo durante este stage** (y oculta el
    token en el log).
  - 57: `echo "${GH_TOKEN}" | docker login ghcr.io -u "${GH_USER}" --password-stdin`
    (nunca la contraseña por argumento).
  - 58-59: `docker push` de las dos etiquetas.
- **Stage `Deploy (compose pull + up)` (65-77)**:
  - 71: `COMPOSE="docker compose -f docker-compose.yml -f docker-compose.cloud.yml -f docker-compose.observability.yml"`
    → los tres overlays del stack cloud (Jenkins **no** se incluye; ya está
    corriendo).
  - 72: `$COMPOSE pull "${SERVICE_NAME}"` → baja la imagen recién pusheada de
    GHCR.
  - 73: `$COMPOSE up -d --no-build "${SERVICE_NAME}"` → recrea **solo ese
    servicio**, sin rebuild (ya está etiquetada).
- **Stage `Smoke` (80-95)** — truco importante: los puertos del stack publican
  en `127.0.0.1` del VPS, **inalcanzables desde la red del contenedor de
  Jenkins**, así que:
  - 86: `docker run --rm --network host` → contenedor efímero en la **red del
    host**.
  - 87-91: `--entrypoint bash`, `--env-file .env`, monta `${STACK_DIR}/scripts`
    en `/scripts:ro` y usa la propia imagen `${REGISTRY}/jenkins:local`;
    ejecuta `cd /scripts && ./wait-for-services.sh && ./smoke.sh` → espera a
    que los servicios levanten y luego verifica respuestas reales. Un fallo
    deja el job en rojo.
- **`post.failure` (97-103)**: imprime instrucciones de **rollback** →
  `TAG=<sha-sano> && docker compose up -d --no-build <svc>` desde `STACK_DIR`
  y apunta a `docs-ea-control/10-devops/ci-cd.md`.

## 6. Flujo completo

```
git push a develop ──webhook──> test-<svc>: Setup → Checkout → Verify ──────────────> (feedback)
git push a main    ──webhook──> test-<svc>: Setup → Checkout → Verify → Deploy gate │
                                                                             │ wait:true
                                                    deploy-<svc>: Checkout → Build image
                                                                    → Push GHCR
                                                                    → compose pull/up
                                                                    → Smoke
```

| Evento | Job | Resultado |
|--------|-----|-----------|
| Push a `develop` | `test-<servicio>` | Feedback de tests |
| Push a `main` | `test-<servicio>` → `deploy-<servicio>` | Test + deploy automático + smoke |
| *Build Now* a mano | cualquiera | Re-ejecución / deploy de un SHA concreto |

## Gotchas

- Los Jenkinsfiles se clonan **desde `main`** del repo infra: cambios en
  `jenkins/**` solo surten efecto tras merge a `main` (+ *Build Now* en `seed`
  si afecta a `jobs.groovy`).
- El token del webhook identifica el job: cada repo tiene su URL
  `...?token=test-<servicio>`.
- Las migraciones de Liquibase son **forward-only**: el rollback de imagen no
  revierte esquema; hace falta un changelog compensatorio (runbook §4.5).
- Local no usa Jenkins: `./up.sh` construye desde el código hermano
  (`REGISTRY=eduaircontrol`, `TAG=local`).

## Ver también

- `docs-ea-control/10-devops/ci-cd.md` — referencia completa (plugins, webhooks,
  credenciales, arranque, rollback).
- `docs-ea-control/05-architecture/decisions/records/ADR-019-jenkins-official-cicd.md`
  — la decisión y sus alternativas.
- `../README.md` — arranque local y puertos del stack.
