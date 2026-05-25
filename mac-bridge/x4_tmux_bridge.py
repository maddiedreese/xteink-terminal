#!/usr/bin/env python3
"""Mirror a tmux pane to the XTeInk X4 terminal firmware."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import textwrap
import time
import unicodedata
import urllib.error
import urllib.request

GLYPH_MAP = str.maketrans(
    {
        "│": "|",
        "┃": "|",
        "║": "|",
        "─": "-",
        "━": "-",
        "═": "-",
        "┌": "+",
        "┐": "+",
        "└": "+",
        "┘": "+",
        "├": "+",
        "┤": "+",
        "┬": "+",
        "┴": "+",
        "┼": "+",
        "╭": "+",
        "╮": "+",
        "╰": "+",
        "╯": "+",
        "█": "#",
        "▓": "#",
        "▒": "#",
        "░": ".",
        "●": "*",
        "•": "*",
        "…": "...",
        "“": '"',
        "”": '"',
        "‘": "'",
        "’": "'",
    }
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--x4", required=True, help="Base URL for the X4, e.g. http://X4_IP_ADDRESS")
    parser.add_argument("--target", default="x4-terminal:", help="tmux target pane/session, e.g. x4-terminal: or x4-terminal:0.0")
    parser.add_argument("--cols", type=int, default=56, help="Expected terminal columns")
    parser.add_argument("--rows", type=int, default=28, help="Rows to capture and send")
    parser.add_argument("--interval", type=float, default=0.25, help="Seconds between tmux checks")
    parser.add_argument("--post-timeout", type=float, default=1.0, help="HTTP timeout for X4 posts")
    return parser.parse_args()


def run_tmux(args: list[str]) -> str:
    result = subprocess.run(["tmux", *args], check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return result.stdout


def ensure_target_size(target: str, cols: int, rows: int) -> None:
    subprocess.run(["tmux", "resize-pane", "-t", target, "-x", str(cols), "-y", str(rows)], check=False)


def clean_line(line: str, cols: int) -> str:
    line = line.translate(GLYPH_MAP)
    line = unicodedata.normalize("NFKD", line)
    line = line.encode("ascii", "replace").decode("ascii")
    line = "".join(ch if ch == "\t" or 32 <= ord(ch) <= 126 else " " for ch in line)
    line = line.replace("\t", "    ")
    return line.rstrip()


def wrap_lines(lines: list[str], cols: int, rows: int) -> list[str]:
    wrapped: list[str] = []
    for line in lines:
        clean = clean_line(line, cols)
        if not clean:
            wrapped.append("")
            continue
        chunks = textwrap.wrap(
            clean,
            width=cols,
            break_long_words=True,
            break_on_hyphens=False,
            replace_whitespace=False,
            drop_whitespace=False,
        )
        wrapped.extend(chunk[:cols] for chunk in chunks)
    if len(wrapped) < rows:
        wrapped = [""] * (rows - len(wrapped)) + wrapped
    return wrapped[-rows:]


def capture(target: str, rows: int, cols: int) -> list[str]:
    start = f"-{rows * 2}"
    output = run_tmux(["capture-pane", "-p", "-t", target, "-S", start, "-E", "-"])
    lines = output.splitlines()
    return wrap_lines(lines, cols, rows)


def cursor(target: str) -> list[int]:
    output = run_tmux(["display-message", "-p", "-t", target, "#{cursor_x} #{cursor_y}"])
    try:
        x_text, y_text = output.strip().split()
        return [int(x_text), int(y_text)]
    except ValueError:
        return [0, 0]


def post_frame(base_url: str, payload: dict, timeout: float) -> bool:
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        base_url.rstrip("/") + "/frame",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return 200 <= response.status < 300
    except (OSError, urllib.error.URLError, TimeoutError):
        return False


def main() -> int:
    args = parse_args()
    last_payload: dict | None = None
    last_ok = True

    while True:
        try:
            ensure_target_size(args.target, args.cols, args.rows)
            payload = {
                "cols": args.cols,
                "rows": capture(args.target, args.rows, args.cols),
                "cursor": cursor(args.target),
                "source": "tmux",
                "target": args.target,
            }
        except (subprocess.CalledProcessError, FileNotFoundError) as error:
            print(f"x4 tmux bridge: {error}", file=sys.stderr)
            time.sleep(1.0)
            continue

        if payload != last_payload:
            ok = post_frame(args.x4, payload, args.post_timeout)
            if ok:
                last_payload = payload
            if ok != last_ok:
                status = "connected" if ok else "X4 unavailable"
                print(f"x4 tmux bridge: {status}", file=sys.stderr)
                last_ok = ok

        time.sleep(args.interval)


if __name__ == "__main__":
    raise SystemExit(main())
