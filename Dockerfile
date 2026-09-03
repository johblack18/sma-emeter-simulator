# syntax=docker/dockerfile:1

### Build stage ###
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# fetch the required libspeedwire dependency as a sibling directory
COPY . /src/sma-emeter-simulator
RUN git clone --depth 1 https://github.com/RalfOGit/libspeedwire.git /src/sma-emeter-simulator/libspeedwire

WORKDIR /src/sma-emeter-simulator
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --config Release -j"$(nproc)"

### Runtime stage ###
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/sma-emeter-simulator/build/sma-emeter-simulator /usr/local/bin/sma-emeter-simulator

# emeter packets are sent to a multicast address on the LAN, host networking is required (see docker-compose.yml)
ENTRYPOINT ["/usr/local/bin/sma-emeter-simulator"]
