// Notifica el resultado de un build a Discord via webhook.
// Credencial Jenkins requerida: discord-webhook (secret text con la URL).
//
// Uso desde un Jenkinsfile (cargar en el primer stage, ANTES de cualquier
// checkout que sobrescriba el workspace, y guardar en una variable de script):
//   def notifyDiscord
//   stage('Setup') { steps { script { notifyDiscord = load 'jenkins/notifyDiscord.groovy' } } }
//   post { always { script { notifyDiscord.call(fields: [Servicio: env.SERVICE_NAME]) } } }
//
// Args opcionales:
//   status — default currentBuild.currentResult
//   fields — Map nombre->valor (se muestran como embed fields inline)

import groovy.json.JsonOutput

def call(Map args = [:]) {
  def status = args.status ?: currentBuild.currentResult
  def fields = args.fields ?: [:]
  def color  = [SUCCESS: 3066993, FAILURE: 15158332, UNSTABLE: 16776960, ABORTED: 9807270].get(status, 9807270)
  def emoji  = [SUCCESS: '✅', FAILURE: '❌', UNSTABLE: '⚠️', ABORTED: '⏹'].get(status, '❓')

  def fieldJson = fields.collect { k, v ->
    def value = (v == null || "${v}".trim() == '') ? '—' : "${v}".take(1024)
    [name: "${k}", value: value, inline: true]
  }

  def payload = JsonOutput.toJson([
    username: 'Jenkins CI',
    embeds: [[
      title:    "${emoji} ${env.JOB_NAME} #${env.BUILD_NUMBER} — ${status}",
      url:      env.BUILD_URL,
      color:    color,
      fields:   fieldJson,
      footer:   [text: env.BUILD_URL ?: ''],
      timestamp: new Date().format("yyyy-MM-dd'T'HH:mm:ss'Z'", TimeZone.getTimeZone('UTC'))
    ]]
  ])

  withCredentials([string(credentialsId: 'discord-webhook', variable: 'DISCORD_WEBHOOK')]) {
    writeFile file: 'discord-notify.json', text: payload
    sh 'curl -sS -m 20 -X POST -H "Content-Type: application/json" ' +
       '-d @discord-notify.json "$DISCORD_WEBHOOK" > /dev/null || true'
  }
}
