plugins {
    java
    id("org.jetbrains.intellij.platform") version "2.19.0"
}

group = "org.ripple.rfl"
version = "0.2.6"

repositories {
    mavenCentral()
    intellijPlatform { defaultRepositories() }
}

dependencies {
    intellijPlatform {
        val localIde = providers.environmentVariable("IDEA_HOME").orNull
        if (localIde != null) local(localIde) else intellijIdea("2026.1.5")
    }
}

java {
    sourceCompatibility = JavaVersion.VERSION_21
    targetCompatibility = JavaVersion.VERSION_21
}

intellijPlatform {
    pluginConfiguration {
        ideaVersion {
            sinceBuild = "261.27258.48"
            untilBuild = "261.*"
        }
    }
}

tasks.register<JavaExec>("checkEditor") {
    dependsOn(tasks.testClasses)
    classpath = sourceSets.test.get().runtimeClasspath
    mainClass = "org.ripple.rfl.EditorTest"
    jvmArgs("-ea", "-Djava.awt.headless=true")
}

tasks.register<JavaExec>("checkRunConfigurations") {
    dependsOn(tasks.testClasses)
    classpath = sourceSets.test.get().runtimeClasspath
    mainClass = "org.ripple.rfl.RunConfigurationTest"
    jvmArgs("-ea", "-Djava.awt.headless=true")
}

tasks.check { dependsOn("checkEditor", "checkRunConfigurations") }
