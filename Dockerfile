FROM gcc:12-bookworm AS build

WORKDIR /src
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates cmake \
    && rm -rf /var/lib/apt/lists/*
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build --parallel

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 python3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 lab
COPY --from=build /src/build/controller_lab /usr/local/bin/controller_lab
COPY python/orchestrator.py /opt/lab/orchestrator.py
USER lab
WORKDIR /opt/lab
ENTRYPOINT ["python3", "/opt/lab/orchestrator.py"]
CMD ["--executable", "/usr/local/bin/controller_lab", "--count", "3"]
