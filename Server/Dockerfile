# Build stage -----------------------------------------------------------------
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# SLikeNet first, on its own layer, so editing server sources does not trigger a
# five-minute rebuild of the networking library.
RUN git clone --depth 1 --branch v.0.1.3 \
      https://github.com/SLikeSoft/SLikeNet.git /opt/slikenet

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY test ./test
COPY tunnel-client ./tunnel-client

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSLIKENET_DIR=/opt/slikenet \
 && cmake --build build -j "$(nproc)"

# Runtime stage ---------------------------------------------------------------
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 crossover

COPY --from=build /src/build/crossover-server     /usr/local/bin/crossover-server
COPY --from=build /src/build/crossover-testclient /usr/local/bin/crossover-testclient
COPY --from=build /src/build/crossover-tunnel     /usr/local/bin/crossover-tunnel

# Optional Railway volume mount point for the remembered rendezvous point.
RUN mkdir -p /data && chown crossover:crossover /data
USER crossover

ENV CROSSOVER_GAME_PORT=60000 \
    CROSSOVER_TUNNEL_PORT=60001 \
    CROSSOVER_STATUS_PORT=8080 \
    CROSSOVER_MAX_PLAYERS=8

EXPOSE 60001/tcp 8080/tcp

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s \
  CMD curl -fsS http://127.0.0.1:${CROSSOVER_STATUS_PORT}/health || exit 1

ENTRYPOINT ["/usr/local/bin/crossover-server"]
