#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
S=${S:-"192.168.33.173:43069"}
export ANDROID_HOME="/home/foxx/Android/Sdk"
export JAVA_HOME="/usr/lib/jvm/openjdk-bin-21"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"
s="-s ${S}"
WD="$(pwd)"
cd "${SCRIPT_DIR}" || exit
./gradlew assembleDebug
adb connect ${S}
adb ${s} install -r app/build/outputs/apk/debug/app-debug.apk
adb ${s} shell monkey -p com.esp32s3.imusim 1
cd "${WD}" || exit
