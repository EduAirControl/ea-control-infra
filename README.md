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
└── .github/workflows/build-services.yml
```

## Arranque local

Requisito: los repositorios de los servicios clonados como hermanos de `infra/`
(el compose construye con `build: ../<servicio>` y `build: ../ea-control-frontend/apps/web`).

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

Credenciales por defecto del superadmin: `admin@eduaircontrol.com` / `EduAirControl1!`
(configurable en `.env` como `SUPERADMIN_PASSWORD`; cámbiala antes de cualquier despliegue).
| Redis | 6379 | 6379 |
| Prometheus | 9090 | 9090 |
| Grafana | 3001 | 3000 |

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

Sustituye `ghcr.io/eduaircontrol/ea-control-<servicio>:latest` por la imagen publicada en tu registro
(`ghcr.io/<owner>/ea-control-<servicio>:<tag>`), generada por el workflow de CI.

## Observabilidad

```bash
docker compose -f docker-compose.yml -f docker-compose.observability.yml up -d
```

Prometheus scrapea `/actuator/prometheus`; los servicios deben incluir
`spring-boot-starter-actuator` y `micrometer-registry-prometheus` para exponer métricas.
El OTel Collector recibe trazas/métricas/logs en `4317` (gRPC) y `4318` (HTTP).
