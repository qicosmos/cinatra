FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake ninja-build git g++-14 libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

ARG CINATRA_REF=9fa1e1549c5feca6b5524928fedd66205a664b6b
WORKDIR /src/cinatra
RUN git init . \
    && git remote add origin https://github.com/qicosmos/cinatra.git \
    && git fetch --depth 1 origin ${CINATRA_REF} \
    && git checkout --detach FETCH_HEAD \
    && git submodule update --init --depth 1 iguana

WORKDIR /app
COPY CMakeLists.txt main.cpp ./
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
        -DCINATRA_SOURCE_DIR=/src/cinatra \
    && cmake --build build -j$(nproc) \
    && strip build/bin/httparena_server

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 libssl3t64 zlib1g \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /app/build/bin/httparena_server /server
EXPOSE 8080 8081
ENTRYPOINT ["/server"]
