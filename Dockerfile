# Build with Bookworm's compiler so its C++ ABI matches both runtime images.
FROM debian:bookworm-slim AS compiler-build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY compiler/Makefile ./
COPY compiler/include ./include
COPY compiler/src ./src
RUN make

FROM debian:bookworm-slim AS worker
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 coreutils && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/nova
COPY --from=compiler-build /src/build/nova ./nova
COPY compiler/stdlib ./stdlib
ENV NOVA_HOME=/opt/nova
USER 65534:65534
ENTRYPOINT ["/opt/nova/nova"]

FROM node:22-bookworm-slim AS frontend-build
WORKDIR /frontend
COPY frontend/package*.json ./
RUN npm ci
COPY frontend/ ./
ARG VITE_API_URL=""
ENV VITE_API_URL=$VITE_API_URL
RUN npm run build

FROM node:22-bookworm AS backend-deps
WORKDIR /app
COPY server/package*.json ./
RUN npm ci --omit=dev

FROM docker:27-cli AS docker-client

FROM node:22-bookworm-slim AS server
ENV NODE_ENV=production NOVA_EXECUTION_MODE=docker NOVA_TEMP_DIR=/var/lib/novacomp/jobs \
    NOVA_HOME=/opt/nova NOVA_COMPILER_PATH=/opt/nova/nova NOVA_STATIC_DIR=/app/dist
WORKDIR /app
COPY --from=docker-client /usr/local/bin/docker /usr/local/bin/docker
COPY --from=backend-deps /app/node_modules ./node_modules
COPY --from=compiler-build /src/build/nova /opt/nova/nova
COPY compiler/stdlib /opt/nova/stdlib
COPY server/package*.json ./
COPY server/src ./src
COPY server/public ./public
COPY --from=frontend-build /frontend/dist ./dist
RUN mkdir -p _tmp workspace /var/lib/novacomp/jobs && chown -R node:node /app /var/lib/novacomp
USER node
EXPOSE 3001
CMD ["node", "src/server.js"]
