plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.hyperwin.app"
    compileSdk = 35

    buildFeatures { aidl = true }

    defaultConfig {
        applicationId = "dev.hyperwin.app"
        minSdk = 34
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"
        ndk { abiFilters += "arm64-v8a" }
    }

    ndkVersion = "27.2.12479018"
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" } }

    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/jniLibs", "../gpu/work/android")
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
            excludes += setOf(
                "**/ring_test",
                "**/bctest",
                "**/scaledtest",
                "**/libvulkan_*.so",
                "**/libbcn_layer.so",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    // Material Design 3 widgets and dynamic colour for the setup and settings screens.
    implementation("com.google.android.material:material:1.12.0")
}
