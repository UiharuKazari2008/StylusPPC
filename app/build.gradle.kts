import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
}

// Release signing: key.properties (git-ignored) next to the project root points at the keystore.
val keyProps = Properties().apply {
    val f = rootProject.file("key.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}

android {
    namespace = "jp.net.seventhreeseven.wmdroid"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "jp.net.seventhreeseven.wmdroid"
        minSdk = 26
        targetSdk = 36
        versionCode = 18
        versionName = "1.0.8"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        externalNativeBuild {
            cmake { arguments += listOf("-DANDROID_STL=c++_static") }
        }
    }

    signingConfigs {
        create("release") {
            if (keyProps.containsKey("storeFile")) {
                storeFile = file(keyProps.getProperty("storeFile"))
                storePassword = keyProps.getProperty("storePassword")
                keyAlias = keyProps.getProperty("keyAlias")
                keyPassword = keyProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            // The emulator core is unusably slow unoptimised.
            externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" } }
        }
        release {
            isMinifyEnabled = false
            externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" } }
            // The release key when key.properties is present, else the debug key (still installable).
            signingConfig = if (keyProps.containsKey("storeFile")) signingConfigs.getByName("release")
                else signingConfigs.getByName("debug")
        }
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.documentfile)
    implementation(libs.androidx.window)
    implementation(libs.androidx.window.java)
}
