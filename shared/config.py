"""
EuNEx shared configuration — centralized settings for all Python services.
"""

import os

# ── Service ports ──────────────────────────────────────────────────
DASHBOARD_PORT = int(os.environ.get("EUNEX_DASHBOARD_PORT", 8090))
FIX_PORT = int(os.environ.get("EUNEX_FIX_PORT", 9001))
CH_PORT = int(os.environ.get("EUNEX_CH_PORT", 8091))

# ── Service URLs (for inter-service communication) ─────────────────
DASHBOARD_URL = os.environ.get("EUNEX_DASHBOARD_URL", f"http://localhost:{DASHBOARD_PORT}")
CH_URL = os.environ.get("EUNEX_CH_URL", f"http://localhost:{CH_PORT}")

# ── Database paths ─────────────────────────────────────────────────
DATA_DIR = os.environ.get("EUNEX_DATA_DIR", os.path.join(os.path.dirname(__file__), "..", "data"))
DASHBOARD_DB = os.path.join(DATA_DIR, "dashboard.db")
CH_DB = os.path.join(DATA_DIR, "clearing_house.db")

# ── Kafka ──────────────────────────────────────────────────────────
KAFKA_BROKERS = os.environ.get("EUNEX_KAFKA_BROKERS", "localhost:9092")
KAFKA_TOPIC_ORDERS = "eunex.orders"
KAFKA_TOPIC_TRADES = "eunex.trades"
KAFKA_TOPIC_SNAPSHOTS = "eunex.market-data"
KAFKA_TOPIC_CONTROL = "eunex.control"

# ── Symbols ────────────────────────────────────────────────────────
SYMBOLS = {
    1: {"name": "AAPL",  "segment": "EQU", "startPrice": 154.0,  "tickSize": 0.01, "lotSize": 1},
    2: {"name": "MSFT",  "segment": "EQU", "startPrice": 324.0,  "tickSize": 0.01, "lotSize": 1},
    3: {"name": "GOOGL", "segment": "EQU", "startPrice": 141.0,  "tickSize": 0.01, "lotSize": 1},
    4: {"name": "TSLA",  "segment": "EQU", "startPrice": 375.0,  "tickSize": 0.01, "lotSize": 1},
    5: {"name": "NVDA",  "segment": "EQU", "startPrice": 201.0,  "tickSize": 0.01, "lotSize": 1},
    6: {"name": "AMD",   "segment": "EQU", "startPrice": 320.0,  "tickSize": 0.01, "lotSize": 1},
    7: {"name": "ENX",   "segment": "EQU", "startPrice": 146.0,  "tickSize": 0.01, "lotSize": 1},
}

# ── Simulation ─────────────────────────────────────────────────────
SIM_INTERVAL = int(os.environ.get("EUNEX_SIM_INTERVAL", 30))
SIM_ORDERS_PER_ROUND = int(os.environ.get("EUNEX_SIM_ORDERS", 4))

# ── Clearing House ─────────────────────────────────────────────────
CH_MEMBERS = {
    f"MBR{i:02d}": {"capital": 100_000.0} for i in range(1, 11)
}
CH_AI_INTERVAL = int(os.environ.get("EUNEX_CH_AI_INTERVAL", 30))
CH_OBLIGATION_MIN_SECURITIES = 20

# ── AI strategies per member ───────────────────────────────────────
CH_DEFAULT_STRATEGIES = {
    "MBR01": "momentum", "MBR02": "momentum", "MBR03": "momentum",
    "MBR04": "mean_revert", "MBR05": "mean_revert", "MBR06": "mean_revert",
    "MBR07": "random", "MBR08": "random", "MBR09": "random",
    "MBR10": "random",
}
