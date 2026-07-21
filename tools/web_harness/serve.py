#!/usr/bin/env python3
"""Serve Emscripten harnesses with the isolation headers pthreads require."""

import argparse
import functools
import http.server


class IsolatedHarnessHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8105)
    parser.add_argument("--directory", default=".")
    args = parser.parse_args()

    handler = functools.partial(IsolatedHarnessHandler, directory=args.directory)
    server = http.server.ThreadingHTTPServer((args.bind, args.port), handler)
    print(f"Serving isolated harnesses at http://{args.bind}:{args.port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
