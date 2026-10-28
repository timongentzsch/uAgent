# The footprint comparison in a container: µAgent built from this checkout
# beside Claude Code, Codex CLI and omp at pinned versions. See the
# "In a container" part of benchmarks/README.md for the build and run lines.
FROM node:24-bookworm-slim AS frontend
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY web/ ./
RUN npm run build

FROM ubuntu:24.04 AS native
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ make libcurl4-openssl-dev zlib1g-dev libssl-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
COPY --from=frontend /web/dist /src/web/dist
RUN cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Release -DUAGENT_WEB_PUSH=ON -DBUILD_TESTING=OFF \
    && cmake --build /tmp/build --target uagent -j 8 \
    && cmake --install /tmp/build --prefix /opt/uagent --strip

FROM ubuntu:24.04
ARG CODEX_VERSION=0.165.0
ARG BUN_VERSION=1.4.4
ARG OMP_VERSION=18.13.2
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates curl git python3 libcurl4t64 libssl3t64 zlib1g unzip xz-utils \
    && rm -rf /var/lib/apt/lists/*
COPY --from=native /opt/uagent /opt/uagent
RUN useradd -m bench
USER bench
WORKDIR /home/bench
RUN curl -fsSL https://claude.ai/install.sh | bash
RUN mkdir codex && curl -fsSL "https://github.com/openai/codex/releases/download/rust-v${CODEX_VERSION}/codex-package-$(uname -m)-unknown-linux-musl.tar.gz" | tar -xz -C codex
RUN curl -fsSL https://bun.sh/install | bash -s "bun-v${BUN_VERSION}"
RUN mkdir omp && cd omp && ~/.bun/bin/bun add "@oh-my-pi/pi-coding-agent@${OMP_VERSION}"
ENV PATH=/home/bench/.local/bin:/home/bench/.bun/bin:$PATH
COPY benchmarks/compare_agents.py benchmarks/footprint-run.sh /opt/bench/
ENTRYPOINT ["bash", "/opt/bench/footprint-run.sh"]
