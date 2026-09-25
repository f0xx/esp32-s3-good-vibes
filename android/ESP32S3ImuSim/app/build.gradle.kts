plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Desk default stays below cloud OTA 00.0001.0000.00001 (versionCode 1000000001).
// Cloud builder sets OTA_VERSION_NAME / OTA_VERSION_CODE (separate from firmware).
val otaVersionName: String = System.getenv("OTA_VERSION_NAME")?.trim().orEmpty()
    .ifEmpty { "1.35.2-ota-erase" }
val otaVersionCode: Int = System.getenv("OTA_VERSION_CODE")?.toIntOrNull()?.takeIf { it > 0 } ?: 95
val otaChannelUrlDefault: String =
    "https://cdn.f0xx.org/good_vibes/v0/ota/channel/stable.json"

android {
    namespace = "com.esp32s3.imusim"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.esp32s3.imusim"
        minSdk = 26
        targetSdk = 34
        versionCode = otaVersionCode
        versionName = otaVersionName
        buildConfigField("String", "PROTO_VERSION_STRING", "\"00.01.00.0001\"")
        buildConfigField("String", "OTA_CHANNEL_URL_DEFAULT", "\"$otaChannelUrlDefault\"")
    }

    signingConfigs {
        getByName("debug") {
            val ksPath = System.getenv("IMU_DEBUG_KEYSTORE")?.trim().orEmpty()
                .ifEmpty { "${System.getProperty("user.home")}/.android/debug.keystore" }
            storeFile = file(ksPath)
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {
        debug {
            signingConfig = signingConfigs.getByName("debug")
        }
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
        // Events has Kotlin default methods; without this, activities that don't
        // override a newly added callback crash with AbstractMethodError on ART.
        freeCompilerArgs += listOf("-Xjvm-default=all")
    }

    buildFeatures {
        aidl = true
        buildConfig = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.work:work-runtime-ktx:2.9.1")
}
