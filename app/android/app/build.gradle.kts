plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

// Android pins live in the repo's tools/versions.env, shared with bootstrap,
// doctor and CI. The NDK matters most: micnote_core's build hook compiles
// libmicnote with whichever NDK this selects.
val versions: Map<String, String> =
    rootProject.file("../../tools/versions.env").readLines()
        .mapNotNull { Regex("""^([A-Z0-9_]+)=(\S*)""").find(it) }
        .associate { it.groupValues[1] to it.groupValues[2] }

android {
    namespace = "dev.micnote.micnote"
    compileSdk = versions.getValue("ANDROID_PLATFORM").toInt()
    ndkVersion = versions.getValue("ANDROID_NDK_VERSION")
    buildToolsVersion = versions.getValue("ANDROID_BUILD_TOOLS")

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        applicationId = "dev.micnote.micnote"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        minSdk = flutter.minSdkVersion
        targetSdk = flutter.targetSdkVersion
        // Uses the version code from pubspec.yaml. When using split APKs, 1000 * ABI_VERSION
        // is added automatically by Flutter. (https://developer.android.com/studio/build/configure-apk-splits#configure-APK-versions)
        // You can force using the value of versionCode by specifying the `-P force-version-code-ignoring-abi=true`
        // flag during build.
        versionCode = flutter.versionCode
        versionName = flutter.versionName
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}
