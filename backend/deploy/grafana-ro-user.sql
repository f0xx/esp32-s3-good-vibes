-- Read-only Grafana user (idempotent). Password substituted by ensure-grafana.sh.
DO $$
BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'imu_grafana_ro') THEN
    CREATE ROLE imu_grafana_ro WITH LOGIN PASSWORD '__GRAFANA_DB_PASSWORD__';
  ELSE
    ALTER ROLE imu_grafana_ro WITH PASSWORD '__GRAFANA_DB_PASSWORD__';
  END IF;
END
$$;

GRANT CONNECT ON DATABASE imu TO imu_grafana_ro;
GRANT USAGE ON SCHEMA public TO imu_grafana_ro;
GRANT SELECT ON ALL TABLES IN SCHEMA public TO imu_grafana_ro;
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT ON TABLES TO imu_grafana_ro;
