# The footprint comparison in a container: µAgent built from this checkout
# beside Claude Code, Codex CLI, omp and OpenCode. Build and run:
# benchmarks/README.md.
ARG UAGENT_STAGE=native
FROM node:24-bookworm-slim AS frontend
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY web/ ./
RUN npm run build

FROM ubuntu:24.04 AS native
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates curl cmake g++ make libcurl4-openssl-dev zlib1g-dev libssl-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
COPY --from=frontend /web/dist /src/web/dist
RUN cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build /tmp/build --target uagent -j 4 \
    && UAGENT_BUILD_DIR=/tmp/build UAGENT_PREFIX=/opt/uagent \
       UAGENT_NONINTERACTIVE=1 UAGENT_REMOTE_BUNDLE="$(test ! -d /src/remote-bundle || echo /src/remote-bundle)" ./install.sh

# Select this stage to measure a published, checksum-verified release.
FROM ubuntu:24.04 AS published
ARG UAGENT_VERSION=1.11.0
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl
COPY install.sh /tmp/install.sh
RUN UAGENT_PREFIX=/opt/uagent UAGENT_NONINTERACTIVE=1 UAGENT_VERSION="$UAGENT_VERSION" sh /tmp/install.sh

FROM ${UAGENT_STAGE} AS installed
FROM ubuntu:24.04
ARG TARGETARCH
ARG CLAUDE_VERSION=2.1.331
ARG CODEX_VERSION=0.172.0
ARG BUN_VERSION=1.4.5
ARG OPENCODE_VERSION=1.19.6
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates curl git openssh-client procps python3 libcurl4t64 libssl3t64 zlib1g unzip xz-utils \
    && rm -rf /var/lib/apt/lists/*
COPY --from=installed /opt/uagent /opt/uagent
RUN useradd -m bench
USER bench
WORKDIR /home/bench
RUN platform=$(echo "$TARGETARCH" | sed s/amd64/x64/) && mkdir -p ~/.local/bin ~/.opencode/bin && \
    curl -fsSL "https://registry.npmjs.org/@anthropic-ai/claude-code-linux-${platform}/-/claude-code-linux-${platform}-${CLAUDE_VERSION}.tgz" | \
    tar -xz -C ~/.local/bin --strip-components=1 package/claude && \
    curl -fsSL "https://github.com/anomalyco/opencode/releases/download/v${OPENCODE_VERSION}/opencode-linux-${platform}.tar.gz" | tar -xz -C ~/.opencode/bin opencode
RUN mkdir codex && curl -fsSL "https://github.com/openai/codex/releases/download/rust-v${CODEX_VERSION}/codex-package-$(uname -m)-unknown-linux-musl.tar.gz" | tar -xz -C codex
RUN curl -fsSL https://bun.sh/install | bash -s "bun-v${BUN_VERSION}"
COPY --chown=bench:bench benchmarks/footprint-omp/ /home/bench/omp/
RUN cd omp && ~/.bun/bin/bun install --frozen-lockfile
ENV PATH=/home/bench/.local/bin:/home/bench/.bun/bin:$PATH
COPY benchmarks/compare_agents.py benchmarks/charts.py benchmarks/scripted_provider.py benchmarks/footprint-run.sh /opt/bench/
ENTRYPOINT ["bash", "/opt/bench/footprint-run.sh"]
