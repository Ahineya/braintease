FROM rust:1.97.1-bookworm@sha256:0e2bcaef56d041a486784e54104a81aebe0da44bd03019bd70bc0401e42e4a97

RUN apt-get update && apt-get install -y --no-install-recommends \
    libasound2-dev libudev-dev libx11-dev libxkbcommon-dev libwayland-dev \
    nodejs python3 make pkg-config && rm -rf /var/lib/apt/lists/*

# Keep first-port validation practical under x86 CPU emulation. These settings
# affect the host tools, not the optimization of RCC-generated guest code.
ENV CARGO_PROFILE_RELEASE_LTO=false \
    CARGO_PROFILE_RELEASE_CODEGEN_UNITS=16 \
    CARGO_PROFILE_RELEASE_OPT_LEVEL=1 \
    CARGO_BUILD_JOBS=4 \
    RCC_NATIVE_HEADLESS=1 RCC_NATIVE_NET=off RCC_NATIVE_AUDIO=off
COPY verify.sh /usr/local/bin/verify-riplix
ENTRYPOINT ["bash", "/usr/local/bin/verify-riplix"]
