-- ============================================================================
-- EduAirControl - puebla environment_monitoring.installation_projection
--
-- POR QUE ESTE PASO ES OBLIGATORIO
--
-- Sin filas en installation_projection, las mediciones que publica el ESP32 se
-- GUARDAN pero no se ven en ninguna parte:
--
--   - EnvironmentMeasurementJpaRepository.latestByEnvironment  -> JOIN projection
--   - EnvironmentMeasurementJpaRepository.history              -> JOIN projection
--   - MeasurementAggregationDao (dashboard y analisis)         -> JOIN projection
--
-- El POST /api/v1/measurements responde 201 igual: MeasurementService no mira la
-- proyeccion, solo inserta. El fallo es silencioso y aparece como "el dashboard
-- esta vacio".
--
-- POR QUE NO HAY NADA QUE LO LLENE SOLO
--
-- La proyeccion esta disenada para alimentarse de los eventos SensorInstalled /
-- SensorRemoved (ver 001_installation_projection.sql, DEC-006 4.3), pero:
--
--   1) ms-sensor-management NO tiene dependencia de RabbitMQ ni publica ningun
--      evento. Es el unico dueño de sensor_installation y no avisa.
--   2) El unico codigo que inserta en la proyeccion es
--      HistoricalBackfillService.PROJECTION_SQL, y esta desactivado por defecto
--      (app.backfill.enabled=false). Ademas su SELECT lee de los esquemas
--      sensors.sensor_installation y classrooms.educational_environment, que en el
--      despliegue por microservicios viven en OTRAS bases de datos
--      (eduaircontrol_sensors y eduaircontrol_classrooms), no en
--      eduaircontrol_monitoring. Por eso tampoco serviria activarlo tal cual.
--
-- Es una Limitacion de la plataforma, no del firmware. La solucion definitiva es
-- que ms-sensor-management publique SensorInstalled por RabbitMQ (o un endpoint de
-- re-sincronizacion en ms-environment-monitoring). Mientras tanto, este INSERT.
--
-- USO
--   psql -h localhost -p 5435 -U monitoring_user -d eduaircontrol_monitoring \
--        -f populate-installation-projection.sql
--
--   Credenciales por defecto: monitorizacion -> ver infra/.env.example
--   (MONITORING_DB / MONITORING_USER / MONITORING_PASSWORD, puerto host 5435)
--
-- SUSTITUIR los <...> por los valores reales. Los tres primeros vienen del paso
-- "Crear sensores, variables e instalaciones" del README (campo sensorInstallationId
-- de la respuesta) y environment_type_id del ambiente (campo environmentTypeId de
-- GET /api/v1/educational-environments).
-- ============================================================================

-- 1) Comprobar que no haya ya filas (idempotencia del script).
-- SELECT sensor_installation_id, educational_environment_id, environment_type_id,
--        sensor_id, synced_at
--   FROM environment_monitoring.installation_projection;

-- 2) Insertar. Los tres UUID deben existir en sensors.sensor_installation
--    (base eduaircontrol_sensors); environment_type_id en
--    classrooms.educational_environment_type (base eduaircontrol_classrooms).
--    removed_at NULL = instalacion activa, que es lo que exigen los JOIN.
INSERT INTO environment_monitoring.installation_projection
    (sensor_installation_id, educational_environment_id,
     environment_type_id, sensor_id, removed_at, synced_at)
VALUES
    ('<SENSOR_INSTALLATION_ID_TEMPERATURE>', '<EDUCATIONAL_ENVIRONMENT_ID>', '<ENVIRONMENT_TYPE_ID>', '<SENSOR_ID_TEMPERATURE>', NULL, now()),
    ('<SENSOR_INSTALLATION_ID_HUMIDITY>',    '<EDUCATIONAL_ENVIRONMENT_ID>', '<ENVIRONMENT_TYPE_ID>', '<SENSOR_ID_HUMIDITY>',    NULL, now()),
    ('<SENSOR_INSTALLATION_ID_NOISE>',       '<EDUCATIONAL_ENVIRONMENT_ID>', '<ENVIRONMENT_TYPE_ID>', '<SENSOR_ID_NOISE>',       NULL, now())
ON CONFLICT (sensor_installation_id) DO UPDATE SET
    educational_environment_id = EXCLUDED.educational_environment_id,
    environment_type_id        = EXCLUDED.environment_type_id,
    sensor_id                  = EXCLUDED.sensor_id,
    removed_at                 = NULL,
    synced_at                  = now();

-- 3) Verificar: deben salir 3 filas.
-- SELECT count(*) FROM environment_monitoring.installation_projection;
--
-- 4) Comprobar que las lecturas del nodo ya se ven. Con esto Deberia devolver
--    3 filas (una por variable) en cuanto el ESP32 haya publicado una muestra:
--
--    curl -s "$GATEWAY/api/v1/environments/$AMBIENTE/current" \
--      -H "Authorization: Bearer $TOKEN" | jq