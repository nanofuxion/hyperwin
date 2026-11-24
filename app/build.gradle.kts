import java.util.Properties

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
        versionCode = 2
        versionName = "0.1.1"
        ndk { abiFilters += "arm64-v8a" }
    }

    // Release signing: keystore.properties (storeFile, storePassword, keyAlias, keyPassword) in the
    // project root or in ~/.hyperwin-release. It is never committed. Without it the release APK is unsigned.
    val keystoreProps = Properties().apply {
        listOf(rootProject.file("keystore.properties"), File(System.getProperty("user.home"), ".hyperwin-release/keystore.properties"))
            .firstOrNull { it.exists() }?.inputStream()?.use { load(it) }
    }
    signingConfigs {
        if (keystoreProps.containsKey("storeFile")) create("release") {
            storeFile = file(keystoreProps.getProperty("storeFile"))
            storePassword = keystoreProps.getProperty("storePassword")
            keyAlias = keystoreProps.getProperty("keyAlias")
            keyPassword = keystoreProps.getProperty("keyPassword")
        }
    }
    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("release")
            isMinifyEnabled = false   // the app reaches AVF by reflection
        }
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
                "**/spvtest",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

// The GPU renderer libraries (libhwgpu, libgfxstream_backend, libc++_shared) are built, not checked
// in. Build them before the app when missing or older than their C++ sources; see gpu/README.md.
val gpuHostLibs by tasks.registering(Exec::class) {
    val lib = rootProject.file("gpu/work/android/arm64-v8a/libhwgpu.so")
    val sources = listOf(
        "gpu/host/hwgpu_host.cpp",
        "gpu/host/hwgpu_host.h",
        "gpu/host/hwgpu_jni.cpp",
        "gpu/host/hwd3d/hwd3d.cpp",
        "gpu/include/hwgpu_ring.h",
        "include/hw_d3d.h",
    ).map { rootProject.file(it) }
    onlyIf { !lib.exists() || sources.any { it.exists() && it.lastModified() > lib.lastModified() } }
    workingDir = rootProject.projectDir
    commandLine("bash", "gpu/host/build-android.sh")
}
tasks.named("preBuild") { dependsOn(gpuHostLibs) }

dependencies {
    // Shizuku grants the two virtual machine permissions once (VmAccess.kt).
    implementation("dev.rikka.shizuku:api:13.1.5")
    implementation("dev.rikka.shizuku:provider:13.1.5")
    // AVF's custom-image and service APIs are hidden @SystemApi classes reached by reflection.
    implementation("org.lsposed.hiddenapibypass:hiddenapibypass:6.1")
    // Material Design 3 widgets and dynamic colour for the setup and settings screens.
    implementation("com.google.android.material:material:1.12.0")
}
