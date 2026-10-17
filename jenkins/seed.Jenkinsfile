// Job "seed" — crea/actualiza los jobs test-* y deploy-* con Job DSL.
//
// Crearlo UNA vez a mano en Jenkins:
//   New Item > Pipeline > nombre "seed"
//   Pipeline > Definition: Pipeline script from SCM
//   SCM: Git -> https://github.com/EduAirControl/ea-control-infra.git
//        Credentials: github-infra   Branch: */main
//   Script Path: jenkins/seed.Jenkinsfile
//
// Lanzar "Build Now" cada vez que cambie jenkins/jobs.groovy.
pipeline {
  agent any
  options { timestamps() }
  stages {
    stage('Seed jobs') {
      steps {
        jobDsl targets: 'jenkins/jobs.groovy',
               removedJobAction: 'DELETE',
               removedViewAction: 'DELETE',
               lookupStrategy: 'JENKINS_ROOT'
      }
    }
  }
}
