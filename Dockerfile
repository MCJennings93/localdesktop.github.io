# Start from the alpine-android image with Android SDK 30 and JDK 17
FROM alvrme/alpine-android:android-30-jdk17 AS build

ENV GRADLE_OPTS="-XX:+UseG1GC -XX:MaxGCPauseMillis=1000"

# Install Gradle 8
RUN wget https://services.gradle.org/distributions/gradle-8.14.2-bin.zip -P /tmp
RUN unzip /tmp/gradle-8.14.2-bin.zip -d /opt
RUN ln -s /opt/gradle-8.14.2/bin/gradle /usr/local/bin/gradle

# Install Rust toolchain and cargo
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
RUN source $HOME/.cargo/env && rustup target add aarch64-linux-android

# Install musl-dev libraries
RUN apk add libgcc build-base clang llvm lld

# If running on an ARM based host, install glib compatibility layer
# This installs multi-arch libraries needed to compile in musl
RUN case "$ARCH" in \
    aarch64|armeb|armel|armhf|armv7) \
        apk add gcompat \
        ;; \
    esac

RUN mkdir /app

# Install patched xbuild
COPY ./patches/ /app/patches
RUN source $HOME/.cargo/env && cargo install --path /app/patches/xbuild/xbuild

# Copy the Rust project files into the container
COPY . /app
WORKDIR /app

# Persistent debug signing key, so rebuilt APKs can upgrade an installed one
COPY .signing/debug.keystore /root/.android/debug.keystore

# Compile APK. The caches keep crates, Gradle and incremental build output between builds;
# the APK is copied out of the cached target dir so the artifact stage can export it.
RUN --mount=type=cache,id=ld-cargo-registry,target=/root/.cargo/registry \
    --mount=type=cache,id=ld-cargo-git,target=/root/.cargo/git \
    --mount=type=cache,id=ld-gradle,target=/root/.gradle \
    --mount=type=cache,id=ld-target,target=/app/target \
    source $HOME/.cargo/env \
    && x build --release --platform android --arch arm64 --format apk \
    && mkdir -p /out/x/release/android/gradle/app/build/outputs/apk/debug \
    && cp target/x/release/android/gradle/app/build/outputs/apk/debug/app-debug.apk \
        /out/x/release/android/gradle/app/build/outputs/apk/debug/

# Only the APK leaves the build, so `docker build --output` skips exporting the 10+ GB image
FROM scratch AS artifact
COPY --from=build /out /

