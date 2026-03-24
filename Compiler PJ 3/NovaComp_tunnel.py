"""
novacomp_tunnel.py
Uses native SSH (already works with your Duo setup) instead of paramiko.

Usage:
    python novacomp_tunnel.py
    python novacomp_tunnel.py --no-browser
"""

import subprocess
import sys
import time
import webbrowser
import os

# ── Config ────────────────────────────────────────────────────────────────────

SOL_HOST     = "sol.asu.edu"
SOL_USERNAME = "pmathu14"
LOCAL_PORT   = 3001
REMOTE_PORT  = 3001

LOCAL_UI_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "compiler_ui.html"
)

NO_BROWSER = "--no-browser" in sys.argv

# # ── parse --node argument ─────────────────────────────────────────────────────
# SOL_HOST = "sol.asu.edu"  # default fallback
# for arg in sys.argv[1:]:
#     if arg.startswith("--node="):
#         SOL_HOST = arg.split("=")[1]
#         break

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    print(f"\n  NovaComp Tunnel{'  [--no-browser]' if NO_BROWSER else ''}")
    print(f"  ────────────────────────────────────────")
    print(f"  Starting SSH tunnel to {SOL_HOST}...")
    print(f"  You will be prompted for your ASU password and Duo push.")
    print(f"  ────────────────────────────────────────\n")

    # build the SSH command
    # -L  local port forwarding
    # -N  don't execute a remote command (tunnel only)
    # -o ServerAliveInterval=30  keep tunnel alive
    ssh_cmd = [
        "ssh",
        "-L", f"{LOCAL_PORT}:localhost:{REMOTE_PORT}",
        "-N",
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=3",
        "-o", "ExitOnForwardFailure=yes",
        f"{SOL_USERNAME}@{SOL_HOST}",
    ]

    try:
        # start SSH — this will prompt for password + Duo in the terminal
        # just like a normal ssh command
        process = subprocess.Popen(ssh_cmd)

        # give SSH a moment to authenticate and open the tunnel
        print(f"  Waiting for tunnel to open...")
        time.sleep(8)  # enough time for password + Duo approval

        if process.poll() is not None:
            print(f"\n  SSH exited early — tunnel failed to open.")
            print(f"  Make sure ASU VPN is connected and try again.")
            sys.exit(1)

        print(f"\n  Tunnel open ✓")
        print(f"  ────────────────────────────────────────")
        print(f"  Local   :  localhost:{LOCAL_PORT}")
        print(f"  Remote  :  {SOL_HOST}:{REMOTE_PORT}")
        print(f"  ────────────────────────────────────────")
        print(f"  Make sure server.js is running on SOL:")
        print(f"    ssh {SOL_USERNAME}@{SOL_HOST}")
        print(f"    cd ~/novacomp && node server.js")
        print(f"  ────────────────────────────────────────")
        print(f"\n  Press Ctrl+C to close the tunnel.\n")

        if not NO_BROWSER:
            if os.path.exists(LOCAL_UI_PATH):
                print(f"  Opening compiler UI in browser...")
                webbrowser.open(f"file://{LOCAL_UI_PATH}")
            else:
                print(f"  Could not find compiler_ui.html at: {LOCAL_UI_PATH}")
                print(f"  Open it manually in your browser.")

        # keep script alive while tunnel runs
        process.wait()

    except KeyboardInterrupt:
        print(f"\n\n  Shutting down tunnel...")
        process.terminate()
        print(f"  Tunnel closed.\n")

    except FileNotFoundError:
        print(f"\n  Error: 'ssh' command not found.")
        print(f"  Make sure OpenSSH is installed and in your PATH.")
        sys.exit(1)

    except Exception as e:
        print(f"\n  Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()