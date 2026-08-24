FROM debian:bookworm-slim AS build
# Install build tools AND ca-certificates (required for git to talk to GitHub)
RUN apt-get update && apt-get install -y \
    cmake \
    gcc \
    g++ \
    make \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Clone nats.c inside the container (keeps your submission zip tiny!)
# We use --depth 1 to only download the latest commit, making it super fast.
RUN git clone --branch v3.8.0 --depth 1 https://github.com/nats-io/nats.c.git /nats.c && \
    cd /nats.c && \
    mkdir build && cd build && \
    cmake .. -DNATS_BUILD_STREAMING=OFF -DNATS_BUILD_TLS_USE_OPENSSL=OFF -DNATS_BUILD_TESTS=OFF -DNATS_BUILD_EXAMPLES=OFF -DNATS_BUILD_WITH_TLS=OFF && \
    make -j$(nproc) install

WORKDIR /app
COPY CMakeLists.txt main.cpp quote_logic.h ./
RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)


FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y libssl3 && rm -rf /var/lib/apt/lists/*
COPY --from=build /usr/local/lib/libnats.so* /usr/local/lib/
COPY --from=build /app/build/strategy /usr/local/bin/strategy

ENV LD_LIBRARY_PATH=/usr/local/lib

# Run the strategy (no host dependencies)
CMD ["strategy"]
