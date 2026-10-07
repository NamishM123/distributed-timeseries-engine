# Multi-stage build: compile with the full toolchain, ship a slim runtime.
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make pkg-config \
    protobuf-compiler protobuf-compiler-grpc libprotobuf-dev libgrpc++-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
 && cmake --build build -j"$(nproc)" --target tsdb_node tsdb_router loadgen

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libprotobuf32t64 libgrpc++1.51t64 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/tsdb_node /usr/local/bin/tsdb_node
COPY --from=build /src/build/tsdb_router /usr/local/bin/tsdb_router
COPY --from=build /src/build/loadgen /usr/local/bin/loadgen
EXPOSE 50050 50051
# Default to a node; the router image overrides the entrypoint via k8s args.
ENTRYPOINT ["/usr/local/bin/tsdb_node"]
