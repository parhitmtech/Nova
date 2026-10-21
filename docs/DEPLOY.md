# Continuous deployment to EC2

`.github/workflows/deploy.yml` runs after **Build and test** succeeds for a push to `main` (or when
started by hand from *Actions → Deploy → Run workflow*):

1. **Build and push images** — builds the `worker` and `server` targets of the root `Dockerfile` from the
   exact commit CI tested and pushes them to GitHub Container Registry as
   `ghcr.io/parhitmtech/nova-{server,worker}:<12-char commit>` (and `:latest`).
2. **Deploy to EC2** — copies `deploy/compose.prod.yml` and `deploy/deploy.sh` to `/opt/novacomp` on the
   host and runs `deploy.sh <tag>`: pull the images, restart the server, wait for `/health`. If the new
   version never becomes healthy it puts the previous tag back and the job fails.

Nothing deploys from other branches. The workflow file must be on `main` before it can run at all.

## One-time setup

### 1. EC2 host

Ubuntu 24.04 (any recent Linux with Docker works):

```sh
sudo apt-get update && sudo apt-get install -y docker.io docker-compose-v2 curl
sudo useradd -m -s /bin/bash -G docker deploy            # the account GitHub Actions logs in as
sudo install -d -o deploy -g deploy /opt/novacomp
sudo install -d -o 1000 -g 1000 -m 700 /var/lib/novacomp/jobs   # 1000 = the server image's user
```

Give the instance an IAM role with the S3, DynamoDB and Bedrock permissions the server needs rather than
putting AWS keys on the box. Open only 22 (SSH, ideally restricted) and 443 in the security group.

Docker socket access is root-equivalent on the host: keep this instance dedicated to NovaComp.

### 2. Server configuration — `/opt/novacomp/server.env`

Owned by `deploy`, mode `600`. Same variables as `server/.env.example`; at least:

```sh
JWT_SECRET=<new random 32+ bytes: node -e "console.log(require('crypto').randomBytes(32).toString('hex'))">
NOVA_ALLOWED_ORIGINS=https://your.domain
AWS_REGION=us-east-1
S3_BUCKET=...
DYNAMODB_TABLE_USERS=...
```

`NODE_ENV=production`, `NOVA_EXECUTION_MODE=docker` and the compiler paths are already set in the image;
`deploy.sh` supplies the worker image tag, and `compose.prod.yml` sets `NOVA_WORKER_ID` and `NOVA_TRUST_PROXY`.
In production the server refuses to start unless `NOVA_ALLOWED_ORIGINS` lists only `https://` origins and
`NOVA_WORKER_ID` is set.

### 3. TLS proxy

The server listens on `127.0.0.1:3001` only. Put Caddy or nginx in front for HTTPS. With nginx, turn off
buffering for the streaming endpoints (program output and NovaBot) and allow WebSocket upgrades for
`/repl` and `/terminal`:

```nginx
location / {
    proxy_pass http://127.0.0.1:3001;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection $connection_upgrade;   # map $http_upgrade -> upgrade/close
    proxy_set_header Host $host;
    proxy_buffering off;          # stream /run-stream and /agent/chat as they are produced
    proxy_read_timeout 3600s;     # REPL / terminal sessions
}
```

(The server also sends `X-Accel-Buffering: no` on its streams, so nginx streams them even without
`proxy_buffering off`.)

### 4. GitHub

*Settings → Environments → New environment* named **`production`**, with these secrets:

| Secret | Value |
|---|---|
| `EC2_HOST` | public DNS name or IP of the instance |
| `EC2_USER` | `deploy` |
| `EC2_SSH_KEY` | private key whose public half is in `/home/deploy/.ssh/authorized_keys` (generate a dedicated one: `ssh-keygen -t ed25519 -f novacomp_deploy -N ""`) |
| `EC2_SSH_KNOWN_HOSTS` | output of `ssh-keyscan -t ed25519 <EC2_HOST>` (pins the host key; the deploy refuses unknown hosts) |

Optional: under the same environment, **Required reviewers** makes every deploy wait for an approval
(continuous *delivery*); leave it empty for fully automatic deploys.

The images are private by default; the workflow logs the host in to GHCR with its own short-lived token
for each deploy, so no registry credentials are stored on the host.

## Operating it

- **See what is live:** `cat /opt/novacomp/.env` on the host (the `NOVA_TAG` line), or the latest
  successful *Deploy* run.
- **Roll back by hand:** `bash /opt/novacomp/deploy.sh <older-tag>` (tags are the 12-character commit
  hashes listed under the repository's *Packages*). The host must be logged in to GHCR to pull an image
  it does not already have: `docker login ghcr.io` with a token that has `read:packages`.
- **Logs:** `docker compose -f /opt/novacomp/compose.prod.yml logs -f server`.
