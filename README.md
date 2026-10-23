# infra — EduAirControl

Infraestructura como código del sistema EduAirControl: orquestación local con
Docker Compose, despliegue en Kubernetes, CI/CD y observabilidad.

> Este repositorio **no contiene código de aplicación**. Los microservicios viven
> en sus propios repositorios (`api-gateway`, `ms-security`,
> `ms-classroom-management`, `ms-sensor-management`, ...).

## Estructura

```
infra/
├── up.sh                            # UN punto de entrada: build + arranque + espera
├── down.sh                          # Para el stack (con --volumes borra las BBDD)
├── docker-compose.yml               # Stack local completo (web + gateway + 6 servicios + PG + RabbitMQ + Redis)
├── docker-compose.observability.yml # Prometheus + Grafana + OTel Collector (overlay opcional)
├── .env.example                     # Variables del stack
├── scripts/
│   ├── clone-all.sh                 # Clones the sibling repos with the compose names
│   ├── wait-for-services.sh         # Espera a que los 8 servicios respondan
│   └── smoke.sh                     # Smoke end-to-end a través del gateway
├── deploy/k8s/                      # Manifiestos de producción
│   ├── namespace.yaml
│   ├── configmap.yaml
│   ├── secrets.example.yaml
│   ├── postgres.yaml                # 6 PostgreSQL (uno por servicio)
│   ├── redis.yaml
│   ├── apps.yaml                    # gateway + microservicios
│   └── ingress.yaml
├── observability/                   # prometheus.yml + otel-collector.yaml
├── iot/esp32-node/                  # Firmware PlatformIO del nodo de medición
├── docker-compose.cloud.yml         # Overlay VPS (Caddy TLS, límites, OAuth2 público)
├── docker-compose.jenkins.yml       # Overlay CI/CD: Jenkins (ADR-019)
├── jenkins/                         # Dockerfile + jobs.groovy + Jenkinsfile.test/deploy + seed
├── Caddyfile                        # TLS: app/api/grafana/jenkins.eduaircontrol.tech
└── .github/workflows/ci.yml         # PR check (compose validation); deploy runs on Jenkins (ADR-019)
```

## Arranque local

Requirement: the repositories cloned as siblings of `infra/`, **with their real
names** (the compose builds with `build: ../ea-control-<service>` and
`build: ../ea-control-frontend/apps/web`). The `scripts/clone-all.sh` helper clones
them with those names:

```bash
./scripts/clone-all.sh   # clones the 8 sibling repos with the compose names
```

```bash
./up.sh              # crea .env si falta, construye, levanta y espera a que todo responda
./scripts/smoke.sh   # opcional: smoke end-to-end por el gateway
./down.sh            # para; --volumes borra las bases de datos
```

`up.sh --no-build` reutiliza las imágenes que ya haya construidas.

> **No mezcles composes.** Los `docker-compose.yml` de cada servicio y el de
> `api-gateway/docker-compose.yml` existen para trabajar un servicio aislado.
> Usarlos a la vez que este stack falla: compiten por nombres de contenedor y
> por puertos (8080, 5672, 6379…). `up.sh` avisa si detecta contenedores sueltos.

## Puertos

| Servicio | Puerto host | Puerto contenedor |
|----------|-------------|-------------------|
| web (frontend) | 3000 | 80 |
| api-gateway | 8080 | 8080 |
| ms-security | 8081 | 8081 |
| ms-classroom-management | 3002 | 3002 |
| ms-sensor-management | 3004 | 3004 |
| ms-user-management | 3007 | 3007 |
| ms-environment-monitoring | 3003 | 3003 |
| PostgreSQL (security) | 5437 | 5432 |
| PostgreSQL (classroom) | 5433 | 5432 |
| PostgreSQL (sensor) | 5434 | 5432 |
| PostgreSQL (user-management) | 5438 | 5432 |
| PostgreSQL (monitoring) | 5435 | 5432 |
| PostgreSQL (user-experience) | 5439 | 5432 |
| RabbitMQ | 5672 | 5672 |
| RabbitMQ (panel) | 15672 | 15672 |
| Redis | 6379 | 6379 |
| Prometheus | 9090 | 9090 |
| Grafana | 3001 | 3000 |

Cuenta SUPER_ADMIN sembrada al arrancar: `admin@eduaircontrol.com` con
`SUPERADMIN_PASSWORD` — **obligatoria en `.env`** (sin default; compose falla sin ella,
R-002). Genera una contraseña fuerte y guárdala en tu gestor de contraseñas.

## Despliegue en producción (Kubernetes)

```bash
kubectl apply -f deploy/k8s/namespace.yaml
kubectl apply -f deploy/k8s/configmap.yaml
# Secretos reales desde la bóveda (no subir secrets.example.yaml con valores reales):
kubectl apply -f deploy/k8s/secrets.example.yaml   # solo plantilla
kubectl apply -f deploy/k8s/redis.yaml
kubectl apply -f deploy/k8s/postgres.yaml
kubectl apply -f deploy/k8s/apps.yaml
kubectl apply -f deploy/k8s/ingress.yaml
```

Las imágenes `ghcr.io/eduaircontrol/ea-control-<servicio>:<tag>` las publica el pipeline
de deploy de Jenkins (ADR-019); ver `## CI/CD (Jenkins)` y `docs-ea-control/10-devops/ci-cd.md`.

## CI/CD (Jenkins)

Jenkins es la CI/CD oficial (ADR-019): corre como contenedor en el VPS y automatiza
tests + deploy. Corre completo en `docs-ea-control/10-devops/ci-cd.md`.

```bash
# Arrancar Jenkins (VPS, junto al resto del stack):
docker compose -f docker-compose.yml -f docker-compose.cloud.yml \
                -f docker-compose.observability.yml \
                -f docker-compose.jenkins.yml up -d --build jenkins

# Password inicial:
docker exec ea-control-jenkins cat /var/jenkins_home/secrets/initialAdminPassword
```

- Panel: `https://jenkins.eduaircontrol.tech` (Caddy) o túnel SSH a `localhost:8085`.
- `test-<svc>` en cada push a `develop`/`main`; en `main` verde encadena
  `deploy-<svc>`: build → GHCR → `compose pull/up` → smoke.
- Credenciales Jenkins: `github-infra` (clone de este repo) y `ghcr-bot` (push GHCR).
- After the first green deploy, `.github/workflows/build-services.yml` is deleted.
- Each repo also has a GitHub Actions PR check (`.github/workflows/ci.yml`):
  `./mvnw verify` (or compose validation in `infra`). It is verification only and does
  not deploy (see the note in ADR-019).

## Observabilidad

```bash
docker compose -f docker-compose.yml -f docker-compose.observability.yml up -d
```

Prometheus scrapea `/actuator/prometheus`; los servicios deben incluir
`spring-boot-starter-actuator` y `micrometer-registry-prometheus` para exponer métricas.
El OTel Collector recibe trazas/métricas/logs en `4317` (gRPC) y `4318` (HTTP).
