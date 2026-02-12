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

EXPOSE 7777/udp
ENTRYPOINT ["game_server"]
