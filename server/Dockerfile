# talos_hub container. Build from the repository root:
#   docker build -f server/Dockerfile -t talos-hub .
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential ca-certificates curl git python3 \
 && rm -rf /var/lib/apt/lists/*
ARG BAZELISK_VERSION=v1.27.0
RUN arch="$(dpkg --print-architecture)" \
 && curl -fsSL -o /usr/local/bin/bazel \
      "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-${arch}" \
 && chmod +x /usr/local/bin/bazel
WORKDIR /src
COPY . .
RUN bazel build -c opt //server:talos_hub && cp bazel-bin/server/talos_hub /talos_hub

FROM ubuntu:24.04
RUN useradd --system --uid 10001 talos && mkdir -p /data && chown talos /data
COPY --from=build /talos_hub /usr/local/bin/talos_hub
COPY deploy/config /etc/talos/deploy/config
COPY contracts/manifests /etc/talos/contracts/manifests
ENV TALOS_LISTEN_HOST=0.0.0.0 \
    TALOS_SQLITE_PATH=/data/talos-journal.db \
    TALOS_CONFIG=/etc/talos/deploy/config/hub.dev.yaml \
    PORT=8080
USER talos
VOLUME /data
EXPOSE 8080
ENTRYPOINT ["/bin/sh", "-c", "exec talos_hub --config \"$TALOS_CONFIG\""]
