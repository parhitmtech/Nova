# Build with Bookworm's compiler so its C++ ABI matches both runtime images.
FROM debian:bookworm-slim AS compiler-build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Compiler_PJ_3/*.cc Compiler_PJ_3/*.h ./
COPY Compiler_PJ_3/stdlib ./stdlib
RUN g++ -std=c++17 -O2 -o compiler compiler.cc lexer.cc inputbuf.cc parser2.cc bytecode.cc autograd.cc

FROM debian:bookworm-slim AS worker
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/nova
COPY --from=compiler-build /src/compiler ./compiler
COPY Compiler_PJ_3/stdlib ./stdlib
ENV NOVA_HOME=/opt/nova
USER 65534:65534
ENTRYPOINT ["/opt/nova/compiler"]

FROM node:22-bookworm-slim AS frontend-build
WORKDIR /frontend
COPY novacomp-frontend/package*.json ./
RUN npm ci
COPY novacomp-frontend/ ./
ARG VITE_API_URL=""
ENV VITE_API_URL=$VITE_API_URL
RUN npm run build

FROM node:22-bookworm AS backend-deps
WORKDIR /app
COPY Compiler_PJ_3/package*.json ./
RUN npm ci --omit=dev

FROM docker:27-cli AS docker-client

FROM node:22-bookworm-slim AS server
ENV NODE_ENV=production NOVA_EXECUTION_MODE=docker NOVA_TEMP_DIR=/var/lib/novacomp/jobs
WORKDIR /app
COPY --from=docker-client /usr/local/bin/docker /usr/local/bin/docker
COPY --from=backend-deps /app/node_modules ./node_modules
COPY --from=compiler-build /src/compiler ./compiler
COPY Compiler_PJ_3/*.js Compiler_PJ_3/package*.json Compiler_PJ_3/landing.html ./
COPY Compiler_PJ_3/stdlib ./stdlib
COPY --from=frontend-build /frontend/dist ./dist
RUN mkdir -p _tmp workspace /var/lib/novacomp/jobs && chown -R node:node /app /var/lib/novacomp
USER node
EXPOSE 3001
CMD ["node", "server.js"]
