
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y libssl3 && rm -rf /var/lib/apt/lists/*
RUN apt-get update && apt-get install -y \
    cmake gcc g++ git make libssl-dev \
    && rm -rf /var/lib/apt/lists/*


COPY nats.c /nats.c
RUN cd /nats.c && \
    mkdir build && cd build && \
    cmake .. -DNATS_BUILD_STREAMING=OFF -DNATS_BUILD_TLS_USE_OPENSSL=OFF -DNATS_BUILD_TESTS=OFF -DNATS_BUILD_EXAMPLES=OFF && \
    make -j$(nproc) install

WORKDIR /app
COPY CMakeLists.txt main.cpp ./
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
