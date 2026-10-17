'use strict';
// Every filesystem location the server depends on, resolved in one place.
// Defaults suit a checkout of the monorepo (server/, compiler/, frontend/ side by side);
// the Docker image overrides them with environment variables.
const path = require('node:path');

const SERVER_ROOT = path.resolve(__dirname, '..');
const REPO_ROOT = path.resolve(SERVER_ROOT, '..');
const fromEnv = (name, fallback) => path.resolve(process.env[name] || fallback);

const NOVA_HOME = fromEnv('NOVA_HOME', path.join(REPO_ROOT, 'compiler'));

module.exports = {
  SERVER_ROOT,
  // Directory containing the Nova stdlib/ (passed to the compiler as NOVA_HOME).
  NOVA_HOME,
  // Not derived from NOVA_HOME: a stale global NOVA_HOME must not hide the freshly built binary.
  COMPILER_PATH: fromEnv('NOVA_COMPILER_PATH',
    path.join(REPO_ROOT, 'compiler', 'build', process.platform === 'win32' ? 'nova.exe' : 'nova')),
  // Built frontend (vite build output) served as the SPA.
  STATIC_DIR: fromEnv('NOVA_STATIC_DIR', path.join(REPO_ROOT, 'frontend', 'dist')),
  PUBLIC_DIR: path.join(SERVER_ROOT, 'public'),
  WORKSPACE_DIR: path.join(SERVER_ROOT, 'workspace'),
  TEMP_DIR: fromEnv('NOVA_TEMP_DIR', path.join(SERVER_ROOT, '_tmp')),
};
