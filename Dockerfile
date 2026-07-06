# syntax=docker/dockerfile:1.7

FROM --platform=$TARGETPLATFORM gcc:15.3.0-trixie@sha256:0631c3651ecb4a7f0bb30f3c40d508b0f88043e37dededc9c8ff77d9150989cf AS build
WORKDIR /app

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      cmake libssl-dev libcpp-httplib-dev \
      libboost-json-dev libboost-url-dev libsoci-dev libmariadb-dev && \
    rm -rf /var/lib/apt/lists/*

COPY . .
ARG REVLM_VERSION=""
ARG REVLM_BUILD_DATE="unknown"
ENV REVLM_VERSION="${REVLM_VERSION:-dev}"
ENV REVLM_BUILD_DATE="${REVLM_BUILD_DATE}"
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target revlm -j"$(nproc)" && \
    arch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)" && \
    mkdir -p "/out/usr/lib/${arch}" && \
    cp build/revlm /out/revlm && \
    cp "/usr/lib/${arch}/libmariadb.so.3" "/out/usr/lib/${arch}/libmariadb.so.3" && \
    cp -R "/usr/lib/${arch}/libmariadb3" "/out/usr/lib/${arch}/libmariadb3" && \
    cp "/usr/lib/${arch}/libcpp-httplib.so."* "/out/usr/lib/${arch}/" && \
    cp "/usr/lib/${arch}/libboost_json.so."* "/out/usr/lib/${arch}/" && \
    cp "/usr/lib/${arch}/libboost_url.so."* "/out/usr/lib/${arch}/" && \
    strip /out/revlm

FROM --platform=$TARGETPLATFORM gcr.io/distroless/cc-debian13:nonroot@sha256:d3cda6e91129130d7229a1806b6a73d292ef245ab032da7851907798024cefba
WORKDIR /
COPY --from=build /out/revlm /revlm
COPY --from=build /out/usr/lib /usr/lib
COPY internal/store/migrations /internal/store/migrations

USER nonroot:nonroot
EXPOSE 8080
ENTRYPOINT ["/revlm"]
