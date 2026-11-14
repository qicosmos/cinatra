FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake ninja-build git g++-14 libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/cinatra
COPY include/ include/
COPY iguana/ iguana/
COPY benchmark/httparena/ benchmark/httparena/
ARG HTTPARENA_X86_64_V3=OFF
RUN cmake -S benchmark/httparena -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
        -DHTTPARENA_X86_64_V3=${HTTPARENA_X86_64_V3} \
    && cmake --build build -j$(nproc) \
    && strip build/bin/httparena_server

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 libssl3t64 zlib1g \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/cinatra/build/bin/httparena_server /server
EXPOSE 8080 8081
ENTRYPOINT ["/server"]
