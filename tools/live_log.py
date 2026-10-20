#!/usr/bin/env python3
"""
S5LAndBox Live Log Streamer (Terminal Client)
Connects directly to the S5LAndBox phone web log server over Wi-Fi (no adb needed).

Usage:
    python tools/live_log.py <phone-ip> [port]
    python tools/live_log.py 192.168.1.45
    python tools/live_log.py 192.168.1.45 8080 --category gles
"""

import sys
import json
import argparse
import urllib.request
import urllib.error

# ANSI colors
COLORS = {
    "kernel": "\033[96m",   # Cyan
    "gles":   "\033[92m",   # Green
    "dyld":   "\033[95m",   # Magenta
    "crash":  "\033[91;1m", # Bold Red
    "info":   "\033[93m",   # Yellow
    "reset":  "\033[0m",
    "gray":   "\033[90m",
}

def main():
    parser = argparse.ArgumentParser(description="S5LAndBox Live Inspector (Wi-Fi Terminal Client)")
    parser.add_argument("host", help="Phone IP address (e.g. 192.168.1.45 or 127.0.0.1)")
    parser.add_argument("port", nargs="?", default="8080", help="Port (default: 8080)")
    parser.add_argument("--category", "-c", choices=["all", "kernel", "gles", "dyld", "crash", "info"],
                        default="all", help="Filter by category")
    parser.add_argument("--search", "-s", default="", help="Search filter text")

    args = parser.parse_args()

    # Handle host:port syntax
    if ":" in args.host:
        host, port_str = args.host.split(":", 1)
        port = int(port_str)
    else:
        host = args.host
        port = int(args.port)

    url = f"http://{host}:{port}/events"
    print(f"\033[1mConnecting to S5LAndBox live log at {url}...\033[0m")
    print(f"\033[90mTip: You can also open http://{host}:{port}/ in your web browser!\033[0m\n")

    req = urllib.request.Request(url, headers={"User-Agent": "S5LAndBox-CLI"})

    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            buffer = ""
            for raw_chunk in response:
                chunk = raw_chunk.decode("utf-8", errors="replace")
                buffer += chunk

                while "\n\n" in buffer:
                    event_data, buffer = buffer.split("\n\n", 1)
                    for line in event_data.split("\n"):
                        if line.startswith("data: "):
                            payload = line[6:].strip()
                            try:
                                item = json.loads(payload)
                                cat = item.get("cat", "info")
                                time_str = item.get("time", "")
                                msg = item.get("msg", "")

                                if args.category != "all" and cat != args.category:
                                    continue
                                if args.search and args.search.lower() not in msg.lower():
                                    continue

                                color = COLORS.get(cat, COLORS["info"])
                                reset = COLORS["reset"]
                                gray = COLORS["gray"]

                                print(f"{gray}[{time_str}]{reset} {color}[{cat.upper():<6}]{reset} {msg}")
                                sys.stdout.flush()
                            except Exception:
                                pass
    except urllib.error.URLError as e:
        print(f"\n\033[91mConnection failed to {url}: {e.reason}\033[0m")
        print("Make sure the phone and PC are on the same Wi-Fi network and S5LAndBox is running.")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n\033[90mDisconnected.\033[0m")

if __name__ == "__main__":
    main()
