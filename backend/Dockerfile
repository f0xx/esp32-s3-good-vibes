FROM python:3.12-slim

WORKDIR /app

# artc0 (deploy target) only allows outbound HTTPS (443), not plain HTTP (80) — the base
# image's default apt sources use http://deb.debian.org, which hangs/fails to connect there.
# Rewrite to https before updating (works fine anywhere HTTP would've worked too).
RUN sed -i 's|http://deb.debian.org|https://deb.debian.org|g; s|http://security.debian.org|https://security.debian.org|g' \
        /etc/apt/sources.list.d/debian.sources 2>/dev/null || true && \
    apt-get update && apt-get install -y --no-install-recommends \
    libpq5 \
    && rm -rf /var/lib/apt/lists/*

# deploy-artc0.sh may stage zephyr.elf + xtensa addr2line under deploy/stage/
COPY deploy/stage /tmp/stage
RUN mkdir -p /opt/imu /opt/imu/firmware-elfs /usr/local/bin && \
    if [ -f /tmp/stage/zephyr.elf ]; then cp /tmp/stage/zephyr.elf /opt/imu/zephyr.elf; fi && \
    if [ -f /tmp/stage/xtensa-addr2line ]; then \
      install -m755 /tmp/stage/xtensa-addr2line /usr/local/bin/xtensa-addr2line && \
      ln -sf /usr/local/bin/xtensa-addr2line \
        /usr/local/bin/xtensa-espressif_esp32s3_zephyr-elf-addr2line; \
    fi && \
    rm -rf /tmp/stage
ENV ZEPHYR_ELF_PATH=/opt/imu/zephyr.elf
ENV IMU_FIRMWARE_ELF_DIR=/opt/imu/firmware-elfs
# Optional: mount CDN roots so UI uploads mirror to cdn[0-2]
# ENV OTA_LOCAL_CDN_ROOTS=/mnt/cdn/0:/mnt/cdn/1:/mnt/cdn/2
ENV OTA_CDN_DIRECTOR_BASE=https://cdn.f0xx.org
ENV OTA_CDN_PREFIX=good_vibes

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app
COPY schema ./schema
COPY web ./web

RUN mkdir -p /data
ENV DATABASE_URL=sqlite:////data/imu_backend.db
ENV IMU_API_KEY=dev-change-me

EXPOSE 8080

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8080"]
