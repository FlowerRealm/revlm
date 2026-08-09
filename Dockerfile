# syntax=docker/dockerfile:1.7

# Build on Ubuntu 24.04 so amd64 ODB .debs match the GCC plugin ABI (same as CI)
# and Boost >= 1.83 is available. Runtime is distroless Debian 13 (newer glibc).
FROM --platform=$TARGETPLATFORM ubuntu:24.04 AS build
WORKDIR /app
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      ca-certificates curl git cmake g++ make pkg-config python3 \
      libssl-dev libcpp-httplib-dev \
      libboost-json-dev libboost-url-dev libboost-random-dev \
      default-libmysqlclient-dev zlib1g-dev && \
    rm -rf /var/lib/apt/lists/* && \
    # MariaDB-only trees folded MYSQL_TIME into mysql.h; ODB still #includes mysql_time.h.
    if [ ! -f /usr/include/mysql/mysql_time.h ] && [ -d /usr/include/mysql ]; then \
      printf '%s\n' '#pragma once' '#include <mysql.h>' > /usr/include/mysql/mysql_time.h; \
    elif [ ! -f /usr/include/mariadb/mysql_time.h ] && [ -d /usr/include/mariadb ]; then \
      printf '%s\n' '#pragma once' '#include <mysql.h>' > /usr/include/mariadb/mysql_time.h; \
    fi

SHELL ["/bin/bash", "-c"]

# ODB 2.5.0: Code Synthesis publishes amd64 packages for ubuntu24.04.
# On amd64 install those; on arm64 build runtime + compiler via build2/bpkg.
ARG ODB_VERSION=2.5.0
RUN set -euo pipefail; \
    if [ "${TARGETARCH}" = "amd64" ]; then \
      cd /tmp; \
      base="https://www.codesynthesis.com/download/odb/${ODB_VERSION}/ubuntu/ubuntu24.04/x86_64"; \
      suffix="${ODB_VERSION}-0~ubuntu24.04_amd64"; \
      for pkg in odb libodb libodb-dev libodb-mysql libodb-mysql-dev; do \
        curl -fsSL -O "${base}/${pkg}_${suffix}.deb"; \
      done; \
      apt-get update; \
      apt-get install -y --no-install-recommends ./*.deb; \
      rm -rf /var/lib/apt/lists/* /tmp/*.deb; \
    else \
      curl -fsSL https://download.build2.org/0.17.0/build2-install-0.17.0.sh -o /tmp/build2-install.sh; \
      sh /tmp/build2-install.sh --yes --no-check --trust yes; \
      export PATH="/usr/local/bin:${PATH}"; \
      mkdir -p /tmp/odb-bpkg && cd /tmp/odb-bpkg; \
      bpkg create -d odb-cfg cc config.cxx=g++ config.install.root=/usr/local; \
      cd odb-cfg; \
      bpkg add https://pkg.cppget.org/1/stable; \
      bpkg fetch --trust-yes; \
      bpkg build -y --trust-yes odb libodb libodb-mysql ?sys:libmysqlclient; \
      bpkg install --all; \
      rm -rf /tmp/odb-bpkg /tmp/build2-install.sh; \
    fi; \
    odb --version; \
    pkg-config --modversion libodb; \
    pkg-config --modversion libodb-mysql

COPY . .
RUN which g++ && which make && g++ --version && \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DREVLM_BUILD_TESTS=OFF && \
    cmake --build build --target revlm revlm_worker -j"$(nproc)" && \
    # The repository pins the companion source as a submodule. Build the
    # native module against this exact full core ABI, then place a target-specific
    # package in the immutable system-plugin directory of this image.
    cmake --install build --prefix /tmp/revlm-plugin-sdk && \
    cmake -S plugins/revlm-plugin -B system-plugin-build \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/tmp/revlm-plugin-sdk && \
    cmake --build system-plugin-build -j"$(nproc)" && \
    for plugin_name in OpenAI Anthropic; do \
      package_file="/tmp/${plugin_name}.revlm-plugin"; \
      python3 plugins/revlm-plugin/packaging/build-package.py "$plugin_name" \
        --system-target "linux-${TARGETARCH}" \
        --module "system-plugin-build/plugins/${plugin_name}/lib${plugin_name}.so" \
        --output "$package_file"; \
      python3 plugins/revlm-plugin/packaging/install-system-package.py "$package_file" \
        --root /out/usr/share/revlm/plugins; \
    done && \
    arch="$(gcc -print-multiarch)" && \
    mkdir -p "/out/usr/lib/${arch}" && \
    cp build/backend/revlm /out/revlm && \
    cp build/backend/revlm-worker /out/revlm-worker && \
    # cmake install may already place librevlm_core.so in this staging path. \
    if [ ! -e "/out/usr/lib/${arch}/librevlm_core.so" ]; then cp -a build/backend/librevlm_core.so* "/out/usr/lib/${arch}/"; fi && \
    # This directory is copied with the runtime UID below. A named Docker
    # volume mounted here therefore survives restarts and is writable by the
    # non-root distroless process.
    mkdir -p /out/var/lib/revlm/plugins && \
    # Copy direct + transitive shared libs (ldd), skip the dynamic linker itself.
    LD_LIBRARY_PATH="/out/usr/lib/${arch}" ldd /out/revlm | grep '=> /' | awk '{print $3}' | sort -u | while read -r lib; do \
      case "$lib" in \
        */ld-linux*.so*) continue ;; \
        */libc.so*|*/libm.so*|*/libdl.so*|*/libpthread.so*|*/librt.so*|*/libgcc_s.so*|*/libstdc++.so*) continue ;; \
      esac; \
      dest="/out/usr/lib/${arch}/$(basename "$lib")"; \
      if [ "$lib" != "$dest" ]; then cp -L "$lib" "$dest"; fi; \
    done && \
    # cpp-httplib pulls brotli; also copy those if linked indirectly through httplib.
    for lib in /usr/lib/${arch}/libbrotli*.so*; do \
      [ -e "$lib" ] || continue; \
      cp -L "$lib" "/out/usr/lib/${arch}/"; \
    done && \
    strip /out/revlm /out/revlm-worker

FROM --platform=$TARGETPLATFORM ubuntu:24.04 AS runtime
RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates && \
    rm -rf /var/lib/apt/lists/* && \
    useradd --system --uid 65532 --no-create-home nonroot
WORKDIR /
COPY --from=build /out/revlm /revlm
COPY --from=build /out/revlm-worker /revlm-worker
COPY --from=build /out/usr/lib /usr/lib
COPY --from=build /out/usr/share/revlm/plugins /usr/share/revlm/plugins
COPY --chown=nonroot:nonroot --from=build /out/var/lib/revlm/plugins /var/lib/revlm/plugins

USER nonroot:nonroot
EXPOSE 8080
VOLUME ["/var/lib/revlm/plugins"]
ENTRYPOINT ["/revlm"]
