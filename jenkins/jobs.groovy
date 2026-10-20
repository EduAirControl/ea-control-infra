// Job DSL — jobs de CI/CD de EduAirControl (ADR-019).
//
// Se ejecuta desde el job "seed" (ver README de infra y jenkins/seed.Jenkinsfile).
// Tras cambiar este fichero, lanzar "Build Now" en seed.
//
// Mapeo repo <-> servicio (los remotos de GitHub NO siguen un patron):
//   ea-control-<servicio>   para la mayoria
//   ms-environment-monitoring              (sin prefijo ea-control-)
//   ms-user-experience                     (renombrado desde -ms-user-experience)
//   ea-control-frontend   -> servicio "web" (la app vive en apps/web)
//
// Webhook por repositorio (solo evento push):
//   https://jenkins.eduaircontrol.tech/generic-webhook-trigger/invoke?token=test-<servicio>
//   Content-Type: application/json
//
// Credenciales Jenkins requeridas (Manage Jenkins > Credentials):
//   github-infra  — PAT (read:repo) para clonar ea-control-infra desde los jobs
//   ghcr-bot      — usuario/token (write:packages) para push de imágenes a GHCR

def INFRA_REPO  = 'https://github.com/EduAirControl/ea-control-infra.git'
def INFRA_CREDS = 'github-infra'
// Los Jenkinsfiles se leen de main (la config de CI viaja en releases).
def INFRA_BRANCH = 'main'

def services = [
  [name: 'api-gateway',               repo: 'https://github.com/EduAirControl/ea-control-api-gateway.git'],
  [name: 'ms-security',               repo: 'https://github.com/EduAirControl/ea-control-ms-security.git'],
  [name: 'ms-classroom-management',   repo: 'https://github.com/EduAirControl/ea-control-ms-classroom-management.git'],
  [name: 'ms-sensor-management',      repo: 'https://github.com/EduAirControl/ea-control-ms-sensor-management.git'],
  [name: 'ms-user-management',        repo: 'https://github.com/EduAirControl/ea-control-ms-user-management.git'],
  [name: 'ms-environment-monitoring', repo: 'https://github.com/EduAirControl/ms-environment-monitoring.git'],
  [name: 'ms-user-experience',        repo: 'https://github.com/EduAirControl/ea-control-ms-user-experience.git'],
  [name: 'web',                       repo: 'https://github.com/EduAirControl/ea-control-frontend.git'],
]

services.each { svc ->
  pipelineJob("test-${svc.name}") {
    description("Tests de ${svc.name}. Webhook push a develop/main; en main encadena deploy-${svc.name}.")
    logRotator { numToKeep(30) }
    properties { disableConcurrentBuilds() }
    // Mismos nombres que inyecta el webhook (patrón del plugin): permite
    // también "Build with Parameters" manual.
    parameters {
      stringParam('REPO_URL', '', 'Clone URL (lo inyecta el webhook)')
      stringParam('COMMIT_SHA', '', 'Commit a verificar (lo inyecta el webhook)')
      stringParam('REF', '', 'refs/heads/<rama> (lo inyecta el webhook)')
    }
    definition {
      cpsScm {
        scm {
          git {
            remote {
              url(INFRA_REPO)
              credentials(INFRA_CREDS)
            }
            branch("*/${INFRA_BRANCH}")
          }
        }
        scriptPath('jenkins/Jenkinsfile.test')
      }
    }
    triggers {
      genericTrigger {
        genericVariables {
          genericVariable { key 'REPO_URL';   value '$.repository.clone_url' }
          genericVariable { key 'COMMIT_SHA'; value '$.after' }
          genericVariable { key 'REF';        value '$.ref' }
        }
        token("test-${svc.name}")
        causeString('Push a $REF de ' + svc.name)
        // Solo pushes a develop y a main (anclado: no dispara con develop-old/mainline).
        regexpFilterText('$REF')
        regexpFilterExpression('^(refs/heads/develop|refs/heads/main)$')
        printPostContent(false)
      }
    }
  }

  pipelineJob("deploy-${svc.name}") {
    description("Deploy de ${svc.name}: build -> GHCR -> compose pull/up en el VPS -> smoke. Encadenado por test-${svc.name} (main).")
    logRotator { numToKeep(20) }
    properties { disableConcurrentBuilds() }
    parameters {
      stringParam('REPO_URL', '', 'Clone URL del repositorio')
      stringParam('COMMIT_SHA', '', 'Commit a desplegar')
      stringParam('SERVICE_NAME', svc.name, 'Servicio compose (la imagen es ea-control-<name>)')
    }
    definition {
      cpsScm {
        scm {
          git {
            remote {
              url(INFRA_REPO)
              credentials(INFRA_CREDS)
            }
            branch("*/${INFRA_BRANCH}")
          }
        }
        scriptPath('jenkins/Jenkinsfile.deploy')
      }
    }
  }
}
