FROM node:26-bookworm-slim AS frontend
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN --mount=type=cache,target=/root/.npm npm ci --no-audit --no-fund
COPY web/ ./
RUN npm run build

FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a AS native
RUN apt-get -o Acquire::Retries=5 update && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
    ca-certificates cmake g++ make libcurl4-openssl-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
COPY --from=frontend /web/dist /src/web/dist
RUN cmake -S . -B /tmp/uagent-build -DCMAKE_BUILD_TYPE=Release \
    -DUAGENT_WEB=ON -DBUILD_TESTING=OFF \
    && cmake --build /tmp/uagent-build --target uagent -j 4

FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a
RUN apt-get -o Acquire::Retries=5 update && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
    bash ca-certificates curl git ripgrep tini tigervnc-standalone-server \
    xauth fonts-liberation libcurl4 \
    && touch /etc/default/google-chrome \
    && curl -fsSL https://dl.google.com/linux/direct/google-chrome-stable_current_$(dpkg --print-architecture).deb -o /tmp/chrome.deb \
    && apt-get install -y --no-install-recommends /tmp/chrome.deb \
    && rm -f /tmp/chrome.deb \
    && rm -rf /var/lib/apt/lists/*
COPY --from=native /tmp/uagent-build/uagent /usr/local/bin/uagent
RUN groupadd --gid 10001 agent && useradd --uid 10001 --gid 10001 \
    --home-dir /data/agent --shell /bin/bash agent \
    && mkdir -p /data/agent /data/browser /workspaces \
    && chown -R agent:agent /data /workspaces \
    && chmod 0700 /data/agent /data/browser
USER agent
ENV HOME=/data/agent UAGENT_BROWSER_DATA=/data/browser \
    UAGENT_WEB_BIND=0.0.0.0 UAGENT_WEB_PORT=8080
WORKDIR /workspaces
EXPOSE 8080
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["/usr/local/bin/uagent", "--web"]
