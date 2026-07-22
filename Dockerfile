# syntax=docker/dockerfile:1.7

# Build on Debian 12 so amd64 ODB .debs match the GCC plugin ABI and the
# distroless runtime glibc. (gcc:16 / Debian 13 breaks ODB's debian12 plugin.)
FROM --platform=$TARGETPLATFORM debian:12-slim AS build
WORKDIR /app
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      ca-certificates curl cmake g++ pkg-config \
      libssl-dev libcpp-httplib-dev \
      libboost-json-dev libboost-url-dev \
      default-libmysqlclient-dev libmariadb-dev && \
    rm -rf /var/lib/apt/lists/* && \
    # MariaDB connector folded MYSQL_TIME into mysql.h; ODB still #includes mysql_time.h.
    if [ ! -f /usr/include/mysql/mysql_time.h ] && [ -d /usr/include/mysql ]; then \
      printf '%s\n' '#pragma once' '#include <mysql.h>' > /usr/include/mysql/mysql_time.h; \
    elif [ ! -f /usr/include/mariadb/mysql_time.h ] && [ -d /usr/include/mariadb ]; then \
      printf '%s\n' '#pragma once' '#include <mysql.h>' > /usr/include/mariadb/mysql_time.h; \
    fi

# ODB 2.5.0: Code Synthesis publishes amd64 Debian packages only.
# On amd64 install those; on arm64 build runtime + compiler via build2/bpkg.
ARG ODB_VERSION=2.5.0
RUN set -euo pipefail; \
    if [ "${TARGETARCH}" = "amd64" ]; then \
      cd /tmp; \
      base="https://www.codesynthesis.com/download/odb/${ODB_VERSION}/debian/debian12/x86_64"; \
      suffix="${ODB_VERSION}-0~debian12_amd64"; \
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
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
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
    strip /out/revlm

FROM --platform=$TARGETPLATFORM gcr.io/distroless/cc-debian12:nonroot
WORKDIR /
COPY --from=build /out/revlm /revlm
COPY --from=build /out/usr/lib /usr/lib

USER nonroot:nonroot
EXPOSE 8080
ENTRYPOINT ["/revlm"]
