#!/bin/bash
export PATH="/tmp/arduino-cli-bin:$PATH"
SKETCH="/home/foxx/repos/My Projects/espXX/esp32-s3-imu-basics/esp32_s3_imu_basics"
FQBN='esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,FlashMode=qio,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none'
arduino-cli compile -b "$FQBN" "$SKETCH"
arduino-cli upload -p /dev/ttyACM0 -b "$FQBN" "$SKETCH"

