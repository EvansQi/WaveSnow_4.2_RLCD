import argparse
import gzip
import os
import re
import socket
import sqlite3
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from json import JSONDecodeError, dumps, loads
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlparse
from urllib.request import ProxyHandler, Request, build_opener, urlopen

TCP_PUSH_PORT = int(os.environ.get("CODEX_USAGE_TCP_PORT", "8766"))


ROOT = Path(__file__).resolve().parents[1]
DATA_FILE = ROOT / "usage.json"
AUTO_FILE = ROOT / "usage.auto.json"
AUTO_SOURCE_URL = os.environ.get("CODEX_USAGE_SOURCE_URL", "").strip()
AUTO_REFRESH_SECONDS = int(os.environ.get("CODEX_USAGE_REFRESH_SECONDS", "30"))
LOCAL_CODEX_ENABLED = os.environ.get("CODEX_USAGE_LOCAL_CODEX", "1") != "0"
LOCAL_CODEX_LOG_DB = Path(os.environ["USERPROFILE"]) / ".codex" / "logs_2.sqlite"
CODEX_AUTH_FILE = Path(os.environ["USERPROFILE"]) / ".codex" / "auth.json"
CODEX_API_BASE = "https://chatgpt.com/backend-api"
CODEX_DATA_MAX_AGE_SECONDS = int(os.environ.get("CODEX_USAGE_MAX_AGE", "86400"))
CODEX_PROXY = os.environ.get("CODEX_PROXY", "http://127.0.0.1:7897").strip()
QWEATHER_KEY = os.environ.get("QWEATHER_KEY", "").strip()
QWEATHER_LOCATION = os.environ.get("QWEATHER_LOCATION", "").strip()
WEATHER_REFRESH_SECONDS = int(os.environ.get("CODEX_WEATHER_REFRESH", "1800"))

DEFAULT_USAGE = {
    "five_hour_remaining": 99,
    "five_hour_reset": "16:28",
    "week_remaining": 85,
    "week_reset": "05/31",
    "temperature_text": "--.-C",
    "humidity_text": "--%",
    "weather_text": "--",
}


def clamp_percent(value, fallback):
    try:
        return max(0, min(100, int(value)))
    except (TypeError, ValueError):
        return fallback


def compact_metric_text(value, fallback):
    if value is None:
        return fallback
    text = str(value).strip().upper()
    text = "".join(ch for ch in text if ch.isdigit() or ch in ".-+% C")
    return text or fallback


def compact_weather_text(value, fallback):
    if value is None:
        return fallback
    text = str(value).strip()
    text = "".join(ch for ch in text if ch.isascii() and (ch.isalpha() or ch == ' '))
    text = text[:16]
    return text.upper() or fallback


def normalize_usage(raw, fallback=None):
    fallback = fallback or DEFAULT_USAGE
    return {
        "five_hour_remaining": clamp_percent(
            raw.get("five_hour_remaining"),
            fallback["five_hour_remaining"],
        ),
        "five_hour_reset": str(raw.get("five_hour_reset") or fallback["five_hour_reset"]),
        "week_remaining": clamp_percent(
            raw.get("week_remaining"),
            fallback["week_remaining"],
        ),
        "week_reset": str(raw.get("week_reset") or fallback["week_reset"]),
        "temperature_text": compact_metric_text(
            raw.get("temperature_text"),
            fallback["temperature_text"],
        ),
        "humidity_text": compact_metric_text(
            raw.get("humidity_text"),
            fallback["humidity_text"],
        ),
        "weather_text": compact_weather_text(
            raw.get("weather_text"),
            fallback.get("weather_text", "--"),
        ),
    }


def read_json_file(path):
    with path.open("r", encoding="utf-8") as handle:
        return loads(handle.read())


def load_usage():
    if not DATA_FILE.exists():
        save_usage(DEFAULT_USAGE)
    return normalize_usage(read_json_file(DATA_FILE), DEFAULT_USAGE)


def save_usage(data):
    DATA_FILE.write_text(
        dumps(normalize_usage(data), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def fetch_source_url():
    request = Request(AUTO_SOURCE_URL, headers={"User-Agent": "codex-usage-rlcd"})
    with urlopen(request, timeout=10) as response:
        return loads(response.read().decode("utf-8"))


def fetch_weather():
    url = f"https://devapi.qweather.com/v7/weather/now?location={QWEATHER_LOCATION}&key={QWEATHER_KEY}&lang=en"
    req = Request(url, headers={
        "User-Agent": "codex-usage",
        "Accept-Encoding": "identity",
    })
    try:
        with urlopen(req, timeout=10) as resp:
            raw = resp.read()
            try:
                text = raw.decode("utf-8")
            except UnicodeDecodeError:
                text = gzip.decompress(raw).decode("utf-8")
            data = loads(text)
            if data.get("code") == "200":
                return data.get("now", {}).get("text")
            print(f"Weather API error: {data.get('code')}")
            return None
    except (HTTPError, URLError, TimeoutError, OSError, JSONDecodeError) as exc:
        print(f"Weather fetch failed: {exc}")
        return None


def format_reset_time(timestamp_text, fallback):
    try:
        dt = datetime.fromtimestamp(int(timestamp_text))
    except (TypeError, ValueError, OSError):
        return fallback
    return f"{dt.hour:02d}:{dt.minute:02d}"


def format_reset_date(timestamp_text, fallback):
    try:
        dt = datetime.fromtimestamp(int(timestamp_text))
    except (TypeError, ValueError, OSError):
        return fallback
    return f"{dt.month}月{dt.day}日"


def get_codex_access_token():
    if not CODEX_AUTH_FILE.exists():
        return None
    try:
        auth = loads(CODEX_AUTH_FILE.read_text(encoding="utf-8"))
        return auth.get("tokens", {}).get("access_token")
    except (json.JSONDecodeError, OSError):
        return None


def fetch_codex_api_usage():
    token = get_codex_access_token()
    if not token:
        return None
    url = f"{CODEX_API_BASE}/codex/usage"
    headers = {
        "Authorization": f"Bearer {token}",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
        "Accept": "application/json",
        "Accept-Language": "en-US,en;q=0.9",
        "Referer": "https://chatgpt.com/codex",
        "Origin": "https://chatgpt.com",
        "Sec-Ch-Ua": '"Google Chrome";v="131", "Chromium";v="131", "Not_A Brand";v="24"',
        "Sec-Ch-Ua-Mobile": "?0",
        "Sec-Ch-Ua-Platform": '"Windows"',
        "Sec-Fetch-Dest": "empty",
        "Sec-Fetch-Mode": "cors",
        "Sec-Fetch-Site": "same-origin",
    }
    req = Request(url, headers=headers)
    proxy_handler = ProxyHandler({"https": CODEX_PROXY} if CODEX_PROXY else {})
    opener = build_opener(proxy_handler)
    try:
        with opener.open(req, timeout=10) as resp:
            data = loads(resp.read().decode("utf-8"))
    except (HTTPError, URLError, TimeoutError, OSError, JSONDecodeError) as exc:
        print(f"Codex API fetch failed: {exc}")
        return None

    rate_limit = data.get("rate_limit", {})
    primary = rate_limit.get("primary_window", {})
    secondary = rate_limit.get("secondary_window", {})

    used_pct = primary.get("used_percent")
    remaining_pct = (100 - used_pct) if used_pct is not None else None
    window_secs = primary.get("limit_window_seconds", 18000)
    resets_at = primary.get("reset_at")
    if resets_at is None and primary.get("used_percent") is not None:
        resets_at = int(time.time()) + window_secs

    week_used_pct = secondary.get("used_percent")
    week_remaining_pct = (100 - week_used_pct) if week_used_pct is not None else None
    week_resets_at = secondary.get("reset_at")

    result = {}
    if remaining_pct is not None:
        result["five_hour_remaining"] = remaining_pct
    if resets_at is not None:
        result["five_hour_reset"] = format_reset_time(str(int(resets_at)), None)
    if week_remaining_pct is not None:
        result["week_remaining"] = week_remaining_pct
    if week_resets_at is not None:
        result["week_reset"] = format_reset_date(str(int(week_resets_at)), None)

    return result if result else None


def advance_reset_time(reset_timestamp, window_seconds):
    """If the reset time has passed, advance it by window intervals until it's in the future."""
    try:
        reset_ts = int(reset_timestamp)
    except (TypeError, ValueError):
        return None
    now = time.time()
    if reset_ts > now:
        return reset_ts
    # How many full windows have passed since the recorded reset time
    elapsed = now - reset_ts
    intervals = int(elapsed / window_seconds) + 1
    return reset_ts + intervals * window_seconds


def load_local_codex_usage():
    if not LOCAL_CODEX_ENABLED or not LOCAL_CODEX_LOG_DB.exists():
        return None

    try:
        con = sqlite3.connect(f"file:{LOCAL_CODEX_LOG_DB}?mode=ro", uri=True)
        try:
            row = con.execute(
                """
                select ts, feedback_log_body
                from logs
                where feedback_log_body like '%x-codex-primary-used-percent%'
                  and feedback_log_body like '%x-codex-secondary-used-percent%'
                order by id desc
                limit 1
                """
            ).fetchone()
        finally:
            con.close()
    except sqlite3.OperationalError as exc:
        print(f"SQLite read failed: {exc}")
        return None

    if not row:
        return None

    log_ts, text = row[0], row[1]

    def header(name):
        match = re.search(rf'"{re.escape(name)}":\s*"([^"]*)"', text)
        return match.group(1) if match else None

    current = load_usage()
    primary_used = header("x-codex-primary-used-percent")
    secondary_used = header("x-codex-secondary-used-percent")

    if primary_used is None or secondary_used is None:
        return None

    if log_ts and (time.time() - log_ts) > CODEX_DATA_MAX_AGE_SECONDS:
        return None

    # Advance reset times if the recorded windows have already expired
    primary_reset_raw = header("x-codex-primary-reset-at")
    secondary_reset_raw = header("x-codex-secondary-reset-at")
    primary_window_secs = int(header("x-codex-primary-window-minutes") or "300") * 60

    advanced_primary = advance_reset_time(primary_reset_raw, primary_window_secs)
    advanced_secondary = advance_reset_time(secondary_reset_raw, 7 * 24 * 3600)

    return normalize_usage(
        {
            "five_hour_remaining": 100 - clamp_percent(
                primary_used,
                100 - current["five_hour_remaining"],
            ),
            "five_hour_reset": format_reset_time(
                str(advanced_primary) if advanced_primary else primary_reset_raw,
                current["five_hour_reset"],
            ),
            "week_remaining": 100 - clamp_percent(
                secondary_used,
                100 - current["week_remaining"],
            ),
            "week_reset": format_reset_date(
                str(advanced_secondary) if advanced_secondary else secondary_reset_raw,
                current["week_reset"],
            ),
            "temperature_text": current["temperature_text"],
            "humidity_text": current["humidity_text"],
        },
        current,
    )


def auto_refresh_once():
    global _last_weather_fetch
    current = load_usage()

    # Try direct Codex API first (real-time)
    api_usage = fetch_codex_api_usage()
    if api_usage:
        merged = {**current, **api_usage}
        save_usage(merged)
        source = "codex-api"
    elif load_local_codex_usage():
        local_usage = load_local_codex_usage()
        save_usage(local_usage)
        source = "local-codex"
    elif AUTO_FILE.exists():
        save_usage(normalize_usage(read_json_file(AUTO_FILE), current))
        source = "file"
    elif AUTO_SOURCE_URL:
        save_usage(normalize_usage(fetch_source_url(), current))
        source = "url"
    else:
        source = "disabled"

    # Refresh weather periodically
    now = time.time()
    if now - _last_weather_fetch > WEATHER_REFRESH_SECONDS:
        weather = fetch_weather()
        if weather:
            current = load_usage()
            current["weather_text"] = compact_weather_text(weather, current["weather_text"])
            save_usage(current)
        _last_weather_fetch = now

    return source


_last_weather_fetch = 0


def auto_refresh_loop():
    while True:
        try:
            auto_refresh_once()
        except (JSONDecodeError, OSError, URLError, TimeoutError, ValueError) as exc:
            print(f"auto refresh skipped: {exc}")
        time.sleep(AUTO_REFRESH_SECONDS)


def build_dashboard_state():
    usage = load_usage()
    return {
        **usage,
        "clock_text": datetime.now().strftime("%H:%M"),
    }


def build_serial_frame():
    state = build_dashboard_state()
    return (
        f"USAGE|{state['five_hour_remaining']}|{state['five_hour_reset']}|"
        f"{state['week_remaining']}|{state['week_reset']}|{state['clock_text']}|"
        f"{state['weather_text']}\n"
    ).encode("utf-8")


def tcp_push_loop(port, interval_seconds):
    tcp_clients_lock = threading.Lock()
    tcp_clients = []

    def accept_clients(server_sock):
        while True:
            try:
                client_sock, addr = server_sock.accept()
                client_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                with tcp_clients_lock:
                    tcp_clients.append(client_sock)
                print(f"TCP client connected: {addr[0]}:{addr[1]}")
            except OSError:
                break

    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.bind(("0.0.0.0", port))
    server_sock.listen(3)
    print(f"TCP push server listening on port {port}")

    threading.Thread(target=accept_clients, args=(server_sock,), daemon=True).start()

    while True:
        frame = build_serial_frame()
        with tcp_clients_lock:
            clients = list(tcp_clients)
        for client_sock in clients:
            try:
                client_sock.sendall(frame)
            except (OSError, ConnectionError):
                with tcp_clients_lock:
                    if client_sock in tcp_clients:
                        tcp_clients.remove(client_sock)
                try:
                    client_sock.close()
                except OSError:
                    pass
        time.sleep(interval_seconds)


class Handler(BaseHTTPRequestHandler):
    def send_json(self, status, payload):
        body = dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == "/usage.json":
            self.send_json(200, build_dashboard_state())
            return

        if parsed.path == "/refresh":
            source = auto_refresh_once()
            self.send_json(200, {"source": source, "usage": build_dashboard_state()})
            return

        if parsed.path == "/update":
            usage = load_usage()
            query = parse_qs(parsed.query)
            usage["five_hour_remaining"] = clamp_percent(
                query.get("five_hour_remaining", [None])[0],
                usage["five_hour_remaining"],
            )
            usage["week_remaining"] = clamp_percent(
                query.get("week_remaining", [None])[0],
                usage["week_remaining"],
            )
            usage["five_hour_reset"] = query.get(
                "five_hour_reset",
                [usage["five_hour_reset"]],
            )[0]
            usage["week_reset"] = query.get("week_reset", [usage["week_reset"]])[0]

            temperature_text = query.get("temperature_text", [None])[0]
            humidity_text = query.get("humidity_text", [None])[0]
            temperature_c = query.get("temperature_c", [None])[0]
            humidity_percent = query.get("humidity_percent", [None])[0]
            if temperature_c:
                temperature_text = f"{temperature_c}C"
            if humidity_percent:
                humidity_text = f"{humidity_percent}%"

            usage["temperature_text"] = compact_metric_text(
                temperature_text,
                usage["temperature_text"],
            )
            usage["humidity_text"] = compact_metric_text(
                humidity_text,
                usage["humidity_text"],
            )

            save_usage(usage)
            self.send_json(200, build_dashboard_state())
            return

        self.send_json(
            404,
            {
                "error": "not_found",
                "usage": "/usage.json",
                "refresh": "/refresh",
                "update": (
                    "/update?five_hour_remaining=99&week_remaining=85"
                    "&temperature_c=26.3&humidity_percent=61"
                ),
            },
        )


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--tcp-port",
        type=int,
        default=TCP_PUSH_PORT,
        help="TCP server port for WiFi push mode",
    )
    parser.add_argument(
        "--tcp-push-seconds",
        type=float,
        default=5,
        help="Seconds between TCP pushes",
    )
    parser.add_argument(
        "--no-tcp",
        action="store_true",
        help="Disable TCP push mode",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    save_usage(load_usage())
    threading.Thread(target=auto_refresh_loop, daemon=True).start()

    if not args.no_tcp:
        threading.Thread(
            target=tcp_push_loop,
            args=(args.tcp_port, args.tcp_push_seconds),
            daemon=True,
        ).start()

    server = ThreadingHTTPServer(("0.0.0.0", 8765), Handler)
    print("Serving Codex usage at http://0.0.0.0:8765/usage.json")
    print(f"Local Codex logs: {'enabled' if LOCAL_CODEX_ENABLED else 'disabled'}")
    print(f"Codex API: {'enabled' if get_codex_access_token() else '(no auth token)'}")
    print(f"Data max age: {CODEX_DATA_MAX_AGE_SECONDS}s")
    print(f"Weather: QWeather location={QWEATHER_LOCATION} (every {WEATHER_REFRESH_SECONDS}s)")
    print(f"Auto file: {AUTO_FILE}")
    print(f"Auto URL: {AUTO_SOURCE_URL or '(disabled)'}")
    if args.no_tcp:
        print("TCP push: disabled")
    else:
        print(f"TCP push: port {args.tcp_port} every {args.tcp_push_seconds:g}s")
    server.serve_forever()


if __name__ == "__main__":
    main()
