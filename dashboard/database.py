"""
EuNEx Dashboard Database — SQLite persistence for orders, trades, and OHLCV history.
"""

import sqlite3
import threading
import time
import os
import json

_local = threading.local()


def _get_conn(db_path):
    if not hasattr(_local, "connections"):
        _local.connections = {}
    if db_path not in _local.connections:
        os.makedirs(os.path.dirname(db_path), exist_ok=True)
        conn = sqlite3.connect(db_path, check_same_thread=False)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")
        _local.connections[db_path] = conn
    return _local.connections[db_path]


def init_db(db_path):
    conn = _get_conn(db_path)
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS orders (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            order_id INTEGER UNIQUE,
            cl_ord_id TEXT,
            symbol_idx INTEGER,
            symbol TEXT,
            side TEXT,
            order_type TEXT,
            price REAL,
            quantity INTEGER,
            remaining_qty INTEGER,
            status TEXT,
            tif TEXT DEFAULT 'Day',
            source TEXT DEFAULT 'dashboard',
            timestamp REAL,
            created_at TEXT DEFAULT (datetime('now'))
        );

        CREATE INDEX IF NOT EXISTS idx_orders_symbol ON orders(symbol);
        CREATE INDEX IF NOT EXISTS idx_orders_status ON orders(status);
        CREATE INDEX IF NOT EXISTS idx_orders_order_id ON orders(order_id);

        CREATE TABLE IF NOT EXISTS trades (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            trade_id INTEGER,
            symbol_idx INTEGER,
            symbol TEXT,
            price REAL,
            quantity INTEGER,
            buy_order_id INTEGER,
            sell_order_id INTEGER,
            timestamp REAL,
            created_at TEXT DEFAULT (datetime('now'))
        );

        CREATE INDEX IF NOT EXISTS idx_trades_symbol ON trades(symbol);
        CREATE INDEX IF NOT EXISTS idx_trades_timestamp ON trades(timestamp);

        CREATE TABLE IF NOT EXISTS ohlcv (
            symbol TEXT,
            bucket INTEGER,
            open REAL,
            high REAL,
            low REAL,
            close REAL,
            volume INTEGER,
            PRIMARY KEY (symbol, bucket)
        );
    """)
    conn.commit()


def save_order(db_path, order):
    conn = _get_conn(db_path)
    conn.execute("""
        INSERT INTO orders (order_id, cl_ord_id, symbol_idx, symbol, side, order_type,
                            price, quantity, remaining_qty, status, tif, source, timestamp)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(order_id) DO UPDATE SET
            remaining_qty=excluded.remaining_qty,
            status=excluded.status
    """, (
        order.get("orderId"), order.get("clOrdId", ""),
        order.get("symbolIdx"), order.get("symbol"),
        order.get("side"), order.get("orderType", "Limit"),
        order.get("price", 0), order.get("quantity"),
        order.get("remainingQty", order.get("quantity")),
        order.get("status", "New"), order.get("tif", "Day"),
        order.get("source", "dashboard"), order.get("timestamp", time.time()),
    ))
    conn.commit()


def save_trade(db_path, trade):
    conn = _get_conn(db_path)
    conn.execute("""
        INSERT OR IGNORE INTO trades (trade_id, symbol_idx, symbol, price, quantity,
                                       buy_order_id, sell_order_id, timestamp)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        trade.get("tradeId"), trade.get("symbolIdx"),
        trade.get("symbol"), trade.get("price"),
        trade.get("quantity"), trade.get("buyOrderId"),
        trade.get("sellOrderId"), trade.get("timestamp", time.time()),
    ))
    conn.commit()


def record_ohlcv(db_path, symbol, price, quantity, bucket_size=60):
    bucket = int(time.time()) // bucket_size * bucket_size
    conn = _get_conn(db_path)
    row = conn.execute(
        "SELECT open, high, low, close, volume FROM ohlcv WHERE symbol=? AND bucket=?",
        (symbol, bucket)
    ).fetchone()

    if row:
        conn.execute("""
            UPDATE ohlcv SET high=MAX(high, ?), low=MIN(low, ?), close=?, volume=volume+?
            WHERE symbol=? AND bucket=?
        """, (price, price, price, quantity, symbol, bucket))
    else:
        conn.execute("""
            INSERT INTO ohlcv (symbol, bucket, open, high, low, close, volume)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        """, (symbol, bucket, price, price, price, price, quantity))
    conn.commit()


def get_ohlcv(db_path, symbol, period_seconds=3600):
    conn = _get_conn(db_path)
    cutoff = int(time.time()) - period_seconds
    rows = conn.execute(
        "SELECT bucket, open, high, low, close, volume FROM ohlcv "
        "WHERE symbol=? AND bucket>=? ORDER BY bucket",
        (symbol, cutoff)
    ).fetchall()
    return [dict(r) for r in rows]


def get_recent_orders(db_path, limit=50):
    conn = _get_conn(db_path)
    rows = conn.execute(
        "SELECT * FROM orders ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    return [dict(r) for r in rows]


def get_recent_trades(db_path, limit=100):
    conn = _get_conn(db_path)
    rows = conn.execute(
        "SELECT * FROM trades ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    return [dict(r) for r in rows]


def get_trade_stats(db_path, symbol=None):
    conn = _get_conn(db_path)
    if symbol:
        row = conn.execute(
            "SELECT COUNT(*) as count, SUM(quantity) as volume, "
            "AVG(price) as avg_price, MAX(price) as high, MIN(price) as low "
            "FROM trades WHERE symbol=?", (symbol,)
        ).fetchone()
    else:
        row = conn.execute(
            "SELECT COUNT(*) as count, SUM(quantity) as volume "
            "FROM trades"
        ).fetchone()
    return dict(row) if row else {}


def get_active_orders(db_path, symbol=None):
    conn = _get_conn(db_path)
    if symbol:
        rows = conn.execute(
            "SELECT * FROM orders WHERE status IN ('New','PartiallyFilled') AND symbol=? "
            "ORDER BY timestamp", (symbol,)
        ).fetchall()
    else:
        rows = conn.execute(
            "SELECT * FROM orders WHERE status IN ('New','PartiallyFilled') ORDER BY timestamp"
        ).fetchall()
    return [dict(r) for r in rows]
