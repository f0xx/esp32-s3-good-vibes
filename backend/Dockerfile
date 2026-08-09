FROM python:3.12-slim

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
    libpq5 \
    && rm -rf /var/lib/apt/lists/*

# deploy-artc0.sh may stage zephyr.elf + xtensa addr2line under deploy/stage/
COPY deploy/stage /tmp/stage
RUN mkdir -p /opt/imu /usr/local/bin && \
    if [ -f /tmp/stage/zephyr.elf ]; then cp /tmp/stage/zephyr.elf /opt/imu/zephyr.elf; fi && \
    if [ -f /tmp/stage/xtensa-addr2line ]; then \
      install -m755 /tmp/stage/xtensa-addr2line /usr/local/bin/xtensa-addr2line && \
      ln -sf /usr/local/bin/xtensa-addr2line \
        /usr/local/bin/xtensa-espressif_esp32s3_zephyr-elf-addr2line; \
    fi && \
    rm -rf /tmp/stage
ENV ZEPHYR_ELF_PATH=/opt/imu/zephyr.elf

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
