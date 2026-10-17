#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

"""Local dev server for TabStationToo, the PCSX2 WebAssembly page.

Serves a build's bin/ directory (index.html, pcsx2-wasm.js/.wasm/.data) on 127.0.0.1 with the
headers that make the page cross-origin isolated, which SharedArrayBuffer (and so the emulator's
threads) requires:

    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp
    Cross-Origin-Resource-Policy: same-origin

Every response, errors included, carries them, plus Cache-Control: no-store so a rebuilt .wasm is
always picked up.

With --bios-dir DIR the files directly inside DIR are also served, read-only, under /dev-bios/, so
automated runs can open index.html?bios=dev-bios/<file> and boot without picking a file. With
--disc FILE that one disc image is served, read-only, as /dev-disc/<its file name> (nothing else in
its directory is reachable), so automated runs can read a disc by URL. The server only ever binds to
127.0.0.1 and never serves anything outside the web root, the BIOS directory and that one file.
Requests whose Host header is not 127.0.0.1/localhost on this port are refused, so a website cannot
use DNS rebinding to reach the server (and the BIOS or disc files) through its own hostname.

Every file supports single byte ranges (Range: bytes=first-last, first- or -suffix; 206 with
Content-Range, or 416 when the range starts past the end), which the disc worker uses to read
sectors on demand. Multiple ranges, other units and malformed headers are ignored (RFC 9110 allows
it): the whole file is sent with 200.

Usage:
    python3 tools/wasm/serve.py [--port 8080] [--bios-dir DIR] [--disc FILE] [--quiet] [DIRECTORY]
"""

from __future__ import annotations

import argparse
import email.utils
import functools
import http.server
import os
import re
import stat
import sys
import urllib.parse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO_ROOT / "build-wasm" / "bin"
BIND_ADDRESS = "127.0.0.1"
DEV_BIOS_PREFIX = "/dev-bios/"
DEV_DISC_PREFIX = "/dev-disc/"

EXTRA_HEADERS = (
    ("Cross-Origin-Opener-Policy", "same-origin"),
    ("Cross-Origin-Embedder-Policy", "require-corp"),
    ("Cross-Origin-Resource-Policy", "same-origin"),
    ("Cache-Control", "no-store"),
    ("X-Content-Type-Options", "nosniff"),
)

MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".htm": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".map": "application/json",
    ".wasm": "application/wasm",
    ".data": "application/octet-stream",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".woff2": "font/woff2",
    ".txt": "text/plain; charset=utf-8",
    ".md": "text/plain; charset=utf-8",
}

BIOS_USUAL_MIN = 4 * 1024 * 1024
BIOS_USUAL_MAX = 8 * 1024 * 1024


def is_within(path: Path, base: Path) -> bool:
    return path == base or base in path.parents


UNSATISFIABLE = "unsatisfiable"
_RANGE_NUMBER = re.compile(r"[0-9]{1,19}")  # ASCII digits only; str.isdigit() also accepts "²"


def parse_range(header: str, size: int) -> tuple[int, int] | str | None:
    """Reads a Range header for a file of `size` bytes (RFC 9110, section 14).

    Returns (first, last), inclusive and clamped to the file, for one satisfiable byte range;
    UNSATISFIABLE when the range starts at or past the end (416); or None when the header must be
    ignored and the whole file sent: other units, several ranges, or malformed syntax.
    """
    unit, sep, spec = header.partition("=")
    if not sep or unit.strip().lower() != "bytes":
        return None
    spec = spec.strip()
    if "," in spec:
        return None  # several ranges would need multipart/byteranges; ignoring them is allowed
    first_text, dash, last_text = spec.partition("-")
    if not dash:
        return None
    first_text, last_text = first_text.strip(), last_text.strip()
    if not first_text:  # bytes=-N: the last N bytes
        if not _RANGE_NUMBER.fullmatch(last_text):
            return None
        suffix = int(last_text)
        if suffix == 0 or size == 0:
            return UNSATISFIABLE
        return max(0, size - suffix), size - 1
    if not _RANGE_NUMBER.fullmatch(first_text) or (last_text and not _RANGE_NUMBER.fullmatch(last_text)):
        return None
    first = int(first_text)
    if last_text and int(last_text) < first:
        return None  # an invalid range-spec makes the whole field invalid
    if first >= size:
        return UNSATISFIABLE
    return first, min(int(last_text), size - 1) if last_text else size - 1


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    server_version = "pcsx2-wasm-serve"
    sys_version = ""
    protocol_version = "HTTP/1.1"
    # TCP_NODELAY: the headers and a small body are separate writes, and with Nagle the body would
    # wait for the browser's delayed ACK (about 40 ms per 2 KiB disc read on Linux).
    disable_nagle_algorithm = True

    def __init__(self, *args, root: Path, bios_dir: Path | None, disc: Path | None, quiet: bool, **kwargs):
        # Set before super().__init__(), which handles the request immediately.
        self.root = root
        self.bios_dir = bios_dir
        self.disc = disc
        self.quiet = quiet
        self.send_range = (0, 0)  # (offset, length) of the body send_head() prepared
        super().__init__(*args, directory=str(root), **kwargs)

    def version_string(self) -> str:
        return self.server_version

    # Every response goes through end_headers(), including send_error() pages and redirects.
    def end_headers(self) -> None:
        for name, value in EXTRA_HEADERS:
            self.send_header(name, value)
        super().end_headers()

    def log_message(self, format: str, *args) -> None:
        if not self.quiet:
            super().log_message(format, *args)

    def list_directory(self, path):  # never list directories
        self.send_error(404, "Not found")
        return None

    def copyfile(self, source, outputfile) -> None:
        # Sends the part of the file send_head() announced (the whole file, or one byte range).
        offset, length = self.send_range
        try:
            sent = self.connection.sendfile(source, offset, length) if length else 0
            if sent != length:
                self.close_connection = True  # the file shrank: Content-Length can no longer be honoured
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True  # the browser went away mid-download (reload, cancelled fetch)

    def resolve_request(self) -> tuple[Path | None, str, str | None]:
        """Maps the request path to a file. Returns (file, kind, redirect_location), kind being
        "web", "bios" or "disc"."""
        raw_path = urllib.parse.urlsplit(self.path).path
        try:
            decoded = urllib.parse.unquote(raw_path, errors="strict")
        except UnicodeDecodeError:
            return None, "web", None
        if "\x00" in decoded or "\\" in decoded or not decoded.startswith("/"):
            return None, "web", None

        if decoded == DEV_BIOS_PREFIX.rstrip("/") or decoded.startswith(DEV_BIOS_PREFIX):
            if self.bios_dir is None:
                return None, "bios", None
            name = decoded[len(DEV_BIOS_PREFIX):]
            # A plain file name directly inside the BIOS directory: no sub-paths, no dot files.
            if not name or "/" in name or name.startswith("."):
                return None, "bios", None
            candidate = self.bios_dir / name
            try:
                resolved = candidate.resolve(strict=True)
            except (OSError, RuntimeError):
                return None, "bios", None
            if resolved.parent != self.bios_dir or not resolved.is_file():
                return None, "bios", None
            return resolved, "bios", None

        if decoded == DEV_DISC_PREFIX.rstrip("/") or decoded.startswith(DEV_DISC_PREFIX):
            # Exactly one URL exists here: the disc's own file name. Nothing else in its directory.
            if self.disc is None or decoded[len(DEV_DISC_PREFIX):] != self.disc.name:
                return None, "disc", None
            return self.disc, "disc", None

        parts = [p for p in decoded.split("/") if p]
        if any(p in (".", "..") or p.startswith(".") for p in parts):
            return None, "web", None
        candidate = self.root.joinpath(*parts)
        try:
            is_dir = candidate.is_dir()
        except OSError:  # e.g. ENAMETOOLONG
            return None, "web", None
        if is_dir:
            if not raw_path.endswith("/"):
                return None, "web", raw_path + "/"
            candidate = candidate / "index.html"
        try:
            resolved = candidate.resolve(strict=True)
        except (OSError, RuntimeError):
            return None, "web", None
        # resolve() follows symlinks, so a link pointing outside the web root is refused here.
        if not is_within(resolved, self.root) or not resolved.is_file():
            return None, "web", None
        return resolved, "web", None

    def send_head(self):
        host = (self.headers.get("Host") or "").strip().lower()
        if host not in self.server.allowed_hosts:
            self.send_error(421, "Misdirected Request")  # still carries the isolation headers
            return None

        path, kind, redirect = self.resolve_request()
        if redirect is not None:
            self.send_response(301)
            self.send_header("Location", redirect)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        if path is None:
            self.send_error(404, "Not found")
            return None

        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(404, "Not found")
            return None
        try:
            st = os.fstat(f.fileno())
            if not stat.S_ISREG(st.st_mode):
                f.close()
                self.send_error(404, "Not found")
                return None
            size = st.st_size
            last_modified = email.utils.formatdate(st.st_mtime, usegmt=True)
            byte_range = None
            range_header = self.headers.get("Range")
            if range_header is not None:
                # If-Range: serve the range only if the file is still the version the client has.
                # There is no ETag, so only an exact Last-Modified date matches.
                if_range = self.headers.get("If-Range")
                if if_range is None or if_range.strip() == last_modified:
                    byte_range = parse_range(range_header, size)
            if byte_range == UNSATISFIABLE:
                f.close()
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None

            if kind in ("bios", "disc"):
                ctype = "application/octet-stream"
            else:
                ctype = MIME_TYPES.get(path.suffix.lower()) or self.guess_type(str(path))
            if byte_range is None:
                self.send_range = (0, size)
                self.send_response(200)
            else:
                first, last = byte_range
                self.send_range = (first, last - first + 1)
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {first}-{last}/{size}")
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(self.send_range[1]))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Last-Modified", last_modified)
            self.end_headers()
            return f
        except Exception:
            f.close()
            raise


def example_bios(bios_dir: Path) -> str | None:
    names = sorted(p.name for p in bios_dir.iterdir()
                   if p.is_file() and not p.name.startswith(".")
                   and BIOS_USUAL_MIN <= p.stat().st_size <= BIOS_USUAL_MAX)
    if "scph39001.bin" in names:
        return "scph39001.bin"
    return names[0] if names else None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Serve TabStationToo (the PCSX2 WebAssembly page) on 127.0.0.1 with cross-origin isolation headers.")
    parser.add_argument("directory", nargs="?", default=str(DEFAULT_ROOT),
                        help=f"web root to serve (default: {DEFAULT_ROOT.relative_to(REPO_ROOT)})")
    parser.add_argument("--port", type=int, default=8080, help="port on 127.0.0.1 (default: 8080)")
    parser.add_argument("--bios-dir", metavar="DIR",
                        help="also serve the files directly inside DIR, read-only, under /dev-bios/ "
                             "(for index.html?bios=dev-bios/<file>)")
    parser.add_argument("--disc", metavar="FILE",
                        help="also serve this one disc image, read-only, as /dev-disc/<its file name> "
                             "(for automated disc reads; nothing else in its directory is served)")
    parser.add_argument("--quiet", action="store_true", help="do not log each request")
    args = parser.parse_args()

    root = Path(args.directory).expanduser().resolve()
    if not root.is_dir():
        print(f"error: {root} does not exist. Build the page first:\n"
              f"  source ~/emsdk/emsdk_env.sh && ninja -C build-wasm pcsx2-wasm", file=sys.stderr)
        return 1
    if not (root / "index.html").is_file():
        print(f"warning: {root} has no index.html (did the pcsx2-wasm POST_BUILD copy run?)", file=sys.stderr)

    bios_dir = None
    if args.bios_dir:
        bios_dir = Path(args.bios_dir).expanduser().resolve()
        if not bios_dir.is_dir():
            print(f"error: --bios-dir {bios_dir} is not a directory", file=sys.stderr)
            return 1

    disc = None
    if args.disc:
        try:
            disc = Path(args.disc).expanduser().resolve(strict=True)
        except (OSError, RuntimeError):
            disc = None
        if disc is None or not disc.is_file():
            print(f"error: --disc {args.disc} is not a file", file=sys.stderr)
            return 1
        if disc.name.startswith("."):
            print(f"error: --disc {disc}: hidden files are not served", file=sys.stderr)
            return 1

    handler = functools.partial(IsolatedHandler, root=root, bios_dir=bios_dir, disc=disc, quiet=args.quiet)
    try:
        server = http.server.ThreadingHTTPServer((BIND_ADDRESS, args.port), handler)
    except OSError as e:
        print(f"error: cannot listen on {BIND_ADDRESS}:{args.port}: {e.strerror or e} (try --port)", file=sys.stderr)
        return 1
    server.daemon_threads = True

    port = server.server_address[1]
    allowed_hosts = {f"{BIND_ADDRESS}:{port}", f"localhost:{port}"}
    if port == 80:
        allowed_hosts |= {BIND_ADDRESS, "localhost"}  # browsers omit the default port in Host
    server.allowed_hosts = allowed_hosts
    base = f"http://{BIND_ADDRESS}:{port}/"
    print(f"Serving {root}")
    print(f"  {base}")
    # ?bios= and ?disc= values are encoded twice: the page reads them with URLSearchParams (one decode)
    # and then resolves them as URL paths (second decode), so '#', '?', '&', '+' and '%' survive.
    def query_path(prefix: str, name: str) -> str:
        return prefix + urllib.parse.quote(urllib.parse.quote(name), safe="")

    bios_query = None
    if bios_dir is not None:
        print(f"Dev BIOS directory {bios_dir} (read-only, under {DEV_BIOS_PREFIX})")
        name = example_bios(bios_dir)
        if name:
            bios_query = "bios=" + query_path("dev-bios/", name)
            print(f"  {base}index.html?{bios_query}")
    if disc is not None:
        disc_query = "disc=" + query_path(DEV_DISC_PREFIX, disc.name)
        print(f"Dev disc {disc} (read-only, byte ranges)")
        print(f"  {base}{DEV_DISC_PREFIX.lstrip('/')}{urllib.parse.quote(disc.name)}")
        print(f"  {base}index.html?{disc_query}")
        if bios_query:
            print(f"  {base}index.html?{bios_query}&{disc_query}")
    print("Press Ctrl+C to stop.", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
