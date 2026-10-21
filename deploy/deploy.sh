#!/usr/bin/env bash
# Runs on the EC2 host (copied to /opt/novacomp by the deploy workflow):
#   deploy.sh <image-tag>
# Pulls the worker and server images for <image-tag>, restarts the server, waits for /health and,
# if it never becomes healthy, puts the previous tag back and exits non-zero.
set -euo pipefail

TAG="${1:?usage: deploy.sh <image-tag>}"
cd /opt/novacomp
COMPOSE=(docker compose -f compose.prod.yml)

previous=""
[ -f .env ] && previous="$(sed -n 's/^NOVA_TAG=//p' .env)"

use_tag() {
  printf 'NOVA_TAG=%s\nDOCKER_GID=%s\n' "$1" "$(stat -c %g /var/run/docker.sock)" > .env
}

healthy() {
  for _ in $(seq 1 30); do
    if curl -fsS http://127.0.0.1:3001/health | grep -q '"status":"ok"'; then return 0; fi
    sleep 2
  done
  return 1
}

echo "Deploying ${TAG} (previous: ${previous:-none})"
docker pull "ghcr.io/parhitmtech/nova-worker:${TAG}"
use_tag "$TAG"
"${COMPOSE[@]}" pull server
"${COMPOSE[@]}" up -d server

if healthy; then
  echo "Healthy: ${TAG} is live"
  docker image prune -f >/dev/null
  exit 0
fi

echo "Health check failed for ${TAG}" >&2
"${COMPOSE[@]}" logs --tail 50 server >&2 || true
if [ -n "$previous" ]; then
  echo "Rolling back to ${previous}" >&2
  use_tag "$previous"
  "${COMPOSE[@]}" up -d server
  healthy && echo "Rolled back to ${previous}" >&2
fi
exit 1
