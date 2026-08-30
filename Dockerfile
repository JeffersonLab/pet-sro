# syntax=docker/dockerfile:1.7
#
# evio_ejfat_replay -- container build.
#
# Two stages:
#
#   builder   Ubuntu 24.04 + the official E2SAR 0.3.2 Debian package (which
#             carries gRPC 1.74.1, protobuf 31.1.0, Abseil and Boost 1.89.0
#             under /usr/local), then an out-of-source Release build of
#             cpp/ with the unit tests run as a build gate.
#
#   runtime   The same Ubuntu 24.04 base with only the two executables, the
#             shared objects ldd(1) says they actually need, CA certificates
#             and a non-root user. No compiler, no headers, no sources.
#
# amd64 only: JeffersonLab publishes E2SAR binaries for x86_64 alone. Building
# on Apple silicon or another arm64 host therefore needs
# `docker buildx build --platform linux/amd64`; see DOCKER.md.

# ---------------------------------------------------------------------------
# Pinned inputs
# ---------------------------------------------------------------------------
# ubuntu:24.04 as of 2026-08-28. The digest is the multi-arch index digest, so
# it resolves correctly under --platform linux/amd64.
ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517

# E2SAR release artifact. The version, the release tag and the SHA-256 of the
# .deb are all pinned; nothing is fetched from a moving branch.
#
# e2sar_0.3.2_amd64.deb is self-contained -- it ships gRPC, protobuf, Abseil,
# c-ares and Boost alongside libe2sar.a, so the separate e2sar-deps package is
# neither needed nor installable next to it (the two overlap on
# /usr/local/include).
ARG E2SAR_VERSION=0.3.2
ARG E2SAR_RELEASE_TAG=E2SAR-0.3.2-main-ubuntu-24.04
ARG E2SAR_DEB=e2sar_0.3.2_amd64.deb
ARG E2SAR_DEB_SHA256=c0e43c4e8553af5c1bf23e1844418a40b9299a9cccd575ccf49fcda53d1f6e3e


# ===========================================================================
# Stage 1 -- builder
# ===========================================================================
FROM ${UBUNTU_IMAGE} AS builder

ARG E2SAR_VERSION
ARG E2SAR_RELEASE_TAG
ARG E2SAR_DEB
ARG E2SAR_DEB_SHA256

ENV DEBIAN_FRONTEND=noninteractive

# Keep the downloaded .debs so the BuildKit cache mounts below are useful.
RUN rm -f /etc/apt/apt.conf.d/docker-clean && \
    echo 'Binary::apt::APT::Keep-Downloaded-Packages "true";' \
        > /etc/apt/apt.conf.d/keep-cache

# Fail early and legibly rather than 300 MB into a doomed download.
RUN arch="$(dpkg --print-architecture)"; \
    if [ "$arch" != "amd64" ]; then \
        echo "ERROR: E2SAR ${E2SAR_VERSION} is published for amd64 only (this stage is ${arch})." >&2; \
        echo "       Build with: docker buildx build --platform linux/amd64 ..." >&2; \
        exit 1; \
    fi

# Toolchain, plus the three pkg-config modules gRPC's .pc files name in
# Requires.private and the E2SAR package does not bundle: openssl, zlib, re2.
# Without them `pkg-config --exists e2sar` fails and CMake silently configures
# the degraded, EJFAT-less build.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        curl \
        g++ \
        make \
        pkg-config \
        binutils \
        libre2-dev \
        libssl-dev \
        zlib1g-dev

# E2SAR, verified against its pinned checksum before it is unpacked.
RUN --mount=type=cache,target=/opt/dl,sharing=locked \
    set -eux; \
    url="https://github.com/JeffersonLab/E2SAR/releases/download/${E2SAR_RELEASE_TAG}/${E2SAR_DEB}"; \
    if [ ! -f "/opt/dl/${E2SAR_DEB}" ]; then \
        curl --fail --location --silent --show-error --retry 3 \
             --output "/opt/dl/${E2SAR_DEB}.part" "$url"; \
        mv "/opt/dl/${E2SAR_DEB}.part" "/opt/dl/${E2SAR_DEB}"; \
    fi; \
    echo "${E2SAR_DEB_SHA256}  /opt/dl/${E2SAR_DEB}" | sha256sum --check --strict -; \
    dpkg --install "/opt/dl/${E2SAR_DEB}"; \
    ldconfig

# e2sar.pc lands in the multiarch subdirectory; gRPC, protobuf, Abseil and
# c-ares .pc files sit directly in /usr/local/lib/pkgconfig. lib64 is listed
# because other E2SAR release builds use it and an absent directory is free.
ENV PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig

WORKDIR /src
COPY cpp/ ./cpp/

# Out-of-source Release build.
#
# PETSRO_BUILD_ERSAP_ACTOR=OFF: the actor is a dlopen()ed plugin that needs an
# ersap-cpp installation and belongs in $ERSAP_HOME, not in this image.
#
# The configure step is deliberately not silenced -- "EJFAT sending : ON" in
# its summary is the evidence that E2SAR was found, and the guard below turns
# the upstream warning-and-continue into a hard failure so a container can
# never ship a binary that quietly cannot send.
RUN set -eux; \
    cmake -S cpp -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DPETSRO_BUILD_TESTS=ON \
        -DPETSRO_BUILD_ERSAP_ACTOR=OFF \
        2>&1 | tee /tmp/cmake-configure.log; \
    grep -q 'EJFAT sending : ON' /tmp/cmake-configure.log

RUN cmake --build build --parallel "$(nproc)"

# Unit tests as a build gate. They need no network and no capture files.
RUN ctest --test-dir build --output-on-failure

# Bring in the ERSAP plugin shared libraries so the dependency scan below
# covers them. These are dlopen()ed at runtime by the ERSAP DPE and need
# their own /usr/local/ transitive dependencies (Abseil, gRPC, Boost, upb)
# in the runtime image -- none of which appear in ldd of the executables.
COPY ersap/lib/ /tmp/ersap_lib/

# Collect exactly what the runtime stage needs: the two executables and the
# ERSAP plugin .so files, for each scanning the shared objects ldd resolves
# under /usr/local. Each library is copied under the SONAME the loader will
# ask for, dereferenced so no dangling symlink survives the COPY.
#
# LD_LIBRARY_PATH lets ldd resolve libersap.so/libxmsg.so (which live in
# ersap/lib on the host) when scanning the plugins; without it ldd reports
# them as "not found" and skips their transitive /usr/local/ dependencies.
#
# Both executables are stripped. A Release build carries no debug info to
# begin with, so this removes only the symbol table.
RUN set -eux; \
    mkdir -p /out/bin /out/lib; \
    install -m 0755 build/evio_ejfat_replay build/evio_ejfat_recv /out/bin/; \
    export LD_LIBRARY_PATH=/tmp/ersap_lib; \
    { \
      for bin in /out/bin/*; do ldd "$bin"; done; \
      find /tmp/ersap_lib -maxdepth 1 -name '*.so*' -type f | \
          while read -r lib; do ldd "$lib" 2>/dev/null; done; \
    } | awk '/=> \/usr\/local\//{print $1 " " $3}' | sort -u > /tmp/needed.txt; \
    test -s /tmp/needed.txt; \
    while read -r soname path; do \
        cp -L "$path" "/out/lib/$soname"; \
    done < /tmp/needed.txt; \
    strip --strip-unneeded /out/lib/*.so*; \
    strip --strip-unneeded /out/bin/*; \
    echo "staged $(ls /out/lib | wc -l) shared objects"; \
    ls -l /out/bin


# ===========================================================================
# Stage 2 -- runtime
# ===========================================================================
FROM ${UBUNTU_IMAGE} AS runtime

ARG E2SAR_VERSION

LABEL org.opencontainers.image.title="evio_ejfat_replay" \
      org.opencontainers.image.description="Replay recorded PET SRO EVIO captures through an EJFAT load balancer, and reassemble them again (evio_ejfat_recv)." \
      org.opencontainers.image.source="https://github.com/JeffersonLab/pet-sro" \
      org.opencontainers.image.documentation="https://github.com/JeffersonLab/pet-sro/blob/main/DOCKER.md" \
      org.opencontainers.image.vendor="Jefferson Lab" \
      org.opencontainers.image.version="1.0.0" \
      org.opencontainers.image.base.name="docker.io/library/ubuntu:24.04"

# Not an OCI key, but the single most useful thing to be able to read off a
# built image: which E2SAR the binaries were linked against.
LABEL org.jlab.e2sar.version="${E2SAR_VERSION}"

# Runtime dependencies:
#   ca-certificates    -- gRPC TLS validation
#   libre2-10          -- libgrpc runtime dependency
#   openjdk-21-jre-headless -- ERSAP is a Java framework; bin/ scripts invoke
#                           `java` directly, so no JRE means no orchestrator
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    set -eux; \
    rm -f /etc/apt/apt.conf.d/docker-clean; \
    echo 'Binary::apt::APT::Keep-Downloaded-Packages "true";' > /etc/apt/apt.conf.d/keep-cache; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        gettext-base \
        libre2-10 \
        libzmq5 \
        openjdk-21-jre-headless; \
    rm -rf /var/log/apt /var/log/dpkg.log

COPY --from=builder /out/lib/ /usr/local/lib/
COPY --from=builder /out/bin/ /usr/local/bin/

# Refresh the loader cache and prove, in the image itself, that every DT_NEEDED
# entry of both executables resolves. `ldd` exits 0 even when a library is
# missing, so its output is what decides.
RUN set -eux; \
    ldconfig; \
    for bin in /usr/local/bin/evio_ejfat_replay /usr/local/bin/evio_ejfat_recv; do \
        ldd "$bin" > /tmp/ldd.txt; \
        if grep -q 'not found' /tmp/ldd.txt; then \
            echo "ERROR: unresolved libraries for $bin" >&2; \
            grep 'not found' /tmp/ldd.txt >&2; \
            exit 1; \
        fi; \
    done; \
    rm -f /tmp/ldd.txt

# Dedicated unprivileged account with a home directory and bash so that an
# interactive shell session works out of the box. Not --system: the id is
# deliberately above SYS_UID_MAX so it cannot collide with a distribution
# service account.
RUN groupadd --gid 10001 petsro && \
    useradd --uid 10001 --gid 10001 --create-home \
            --shell /bin/bash petsro

# ERSAP runtime tree and the environment setup script. Placed under
# /opt/petsro so that `source env.sh` (which sets ERSAP_HOME=$(pwd)/ersap)
# resolves correctly when the shell starts in that directory.
COPY --chown=10001:10001 env.sh /opt/petsro/env.sh
COPY --chown=10001:10001 ersap/ /opt/petsro/ersap/

# -XX:+UseBiasedLocking was removed in Java 18; Java 21 rejects it as a fatal
# error. Strip it from every ERSAP launcher script.
RUN find /opt/petsro/ersap/bin -type f \
        -exec sed -i 's/-XX:+UseBiasedLocking//g' {} +

# Captures are mounted here read-only, so relative file arguments resolve.
RUN install -d -m 0755 -o petsro -g petsro /data

# Start in the project root so the user can immediately run `source env.sh`
# and `cd ersap` without navigating first.
WORKDIR /opt/petsro

USER 10001:10001

# Interactive bash login shell. The -l flag sources /etc/profile, which puts
# the OpenJDK binaries on PATH so ERSAP's bin/ scripts find `java`.
# To run evio_ejfat_replay non-interactively, override the entrypoint:
#   docker run --entrypoint /usr/local/bin/evio_ejfat_replay <image> [args]
ENTRYPOINT ["/bin/bash"]
CMD ["-l"]

# Provenance, for images that get pushed to a registry. Deliberately last: both
# values change on every commit, and everything after an ARG is cache-busted by
# it, so keeping them here means a new revision rebuilds nothing but metadata.
# Both default to empty, so an unlabelled local build stays fully cached.
ARG VCS_REF=""
ARG BUILD_DATE=""
LABEL org.opencontainers.image.revision="${VCS_REF}" \
      org.opencontainers.image.created="${BUILD_DATE}"
