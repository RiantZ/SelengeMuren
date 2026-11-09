#!/usr/bin/env python3
"""
Serves the WebGL2 metrics POC (tests/ui/poc) over http: the page starts a Web Worker, which file:// does not allow.

Usage:
  python3 tests/ui/scripts/serve.py                  # http://127.0.0.1:8765/
  python3 tests/ui/scripts/serve.py --open           # and open it in the default browser
  python3 tests/ui/scripts/serve.py --port 9000
  python3 tests/ui/scripts/serve.py --host 0.0.0.0   # reachable from other machines in the LAN
"""

import argparse
import functools
import http.server
import webbrowser
from pathlib import Path

POC_DIR = Path(__file__).parent.parent.resolve() / "poc"


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    """Static files without caching, so an edited page reloads as is."""

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, format: str, *args) -> None:
        pass


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve the WebGL2 metrics POC")
    parser.add_argument("--host", default="127.0.0.1", help="address to listen on (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8765, help="port to listen on (default: 8765)")
    parser.add_argument("--open", action="store_true", help="open the page in the default browser")
    args = parser.parse_args()

    handler = functools.partial(NoCacheHandler, directory=str(POC_DIR))
    with http.server.ThreadingHTTPServer((args.host, args.port), handler) as server:
        url = f"http://{'127.0.0.1' if args.host == '0.0.0.0' else args.host}:{args.port}/"
        print(f"POC: {url}  (Ctrl+C to stop)", flush=True)
        if args.open:
            webbrowser.open(url)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
