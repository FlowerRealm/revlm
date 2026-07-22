# syntax=docker/dockerfile:1.7

# Build on Ubuntu 24.04 so amd64 ODB .debs match the GCC plugin ABI (same as CI)
# and Boost >= 1.83 is available. Runtime is distroless Debian 13 (newer glibc).
FROM --platform=$TARGETPLATFORM ubuntu:24.04 AS build
WORKDIR /app
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      ca-certificates curl git cmake g++ make pkg-config \
      libssl-dev libcpp-httplib-dev \
      libboost-json-dev libboost-url-dev \
      default-libmysqlclient-dev && \
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
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target revlm -j"$(nproc)" && \
    arch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)" && \
    mkdir -p "/out/usr/lib/${arch}" && \
    cp build/revlm /out/revlm && \
    cp /usr/lib/${arch}/libodb*.so* "/out/usr/lib/${arch}/" 2>/dev/null || \
      cp /usr/local/lib/libodb*.so* "/out/usr/lib/${arch}/" && \
    (cp "/usr/lib/${arch}/libmysqlclient.so."* "/out/usr/lib/${arch}/" 2>/dev/null || \
     cp "/usr/lib/${arch}/libmariadb.so."* "/out/usr/lib/${arch}/" 2>/dev/null || true) && \
    cp "/usr/lib/${arch}/libcpp-httplib.so."* "/out/usr/lib/${arch}/" && \
    cp "/usr/lib/${arch}/libboost_json.so."* "/out/usr/lib/${arch}/" && \
    cp "/usr/lib/${arch}/libboost_url.so."* "/out/usr/lib/${arch}/" && \
    # libxcrypt (libcrypt.so.2) required by password hashing at runtime
    (cp "/usr/lib/${arch}/libcrypt.so."* "/out/usr/lib/${arch}/" 2>/dev/null || true) && \
    strip /out/revlm

FROM --platform=$TARGETPLATFORM gcr.io/distroless/cc-debian13:nonroot@sha256:d97bc0a941b8d4be647dc0ee75b264ddbb772f1ac5ba690a4309c00723b23775
WORKDIR /
COPY --from=build /out/revlm /revlm
COPY --from=build /out/usr/lib /usr/lib

USER nonroot:nonroot
EXPOSE 8080
ENTRYPOINT ["/revlm"]
