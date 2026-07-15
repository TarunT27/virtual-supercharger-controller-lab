FROM gcc:14 AS build
WORKDIR /app
RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake \
    && rm -rf /var/lib/apt/lists/*
COPY . .
RUN cmake -S . -B build && cmake --build build

FROM debian:bookworm-slim
COPY --from=build /app/build/controller_lab /usr/local/bin/controller_lab
CMD ["controller_lab"]
