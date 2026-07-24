FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
RUN make -j$(nproc)

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/game_server /usr/local/bin/game_server

# Ship the active map beside the server binary (default --map=default.map).
# WORKDIR makes the relative default path resolve at runtime.
WORKDIR /app
COPY default.map /app/default.map

EXPOSE 7777/udp
ENTRYPOINT ["game_server"]
