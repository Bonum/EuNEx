"""
EuNEx Clearing House Database — SQLite persistence for members, holdings, settlements.
"""

import sqlite3
import threading
import time
import os

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


def init_db(db_path, members):
    conn = _get_conn(db_path)
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS ch_members (
            member_id TEXT PRIMARY KEY,
            capital REAL,
            created_at TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS ch_holdings (
            member_id TEXT,
            symbol TEXT,
            quantity INTEGER DEFAULT 0,
            avg_cost REAL DEFAULT 0,
            PRIMARY KEY (member_id, symbol)
        );

        CREATE TABLE IF NOT EXISTS ch_daily_trades (
            member_id TEXT,
            trading_date TEXT,
            buy_count INTEGER DEFAULT 0,
            sell_count INTEGER DEFAULT 0,
            total_securities INTEGER DEFAULT 0,
            PRIMARY KEY (member_id, trading_date)
        );

        CREATE TABLE IF NOT EXISTS ch_trade_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            member_id TEXT,
            symbol TEXT,
            side TEXT,
            quantity INTEGER,
            price REAL,
            cl_ord_id TEXT,
            trading_date TEXT,
            timestamp REAL
        );
        CREATE INDEX IF NOT EXISTS idx_trade_log_member
            ON ch_trade_log(member_id, trading_date);

        CREATE TABLE IF NOT EXISTS ch_settlements (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            member_id TEXT,
            trading_date TEXT,
            opening_capital REAL,
            closing_capital REAL,
            realized_pnl REAL,
            unrealized_pnl REAL,
            obligation_met INTEGER DEFAULT 0,
            settled_at TEXT DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_settlements_member
            ON ch_settlements(member_id, trading_date);
    """)

    for mid, info in members.items():
        conn.execute(
            "INSERT OR IGNORE INTO ch_members (member_id, capital) VALUES (?, ?)",
            (mid, info["capital"])
        )
    conn.commit()


def get_member(db_path, member_id):
    conn = _get_conn(db_path)
    row = conn.execute("SELECT * FROM ch_members WHERE member_id=?", (member_id,)).fetchone()
    return dict(row) if row else None


def get_all_members(db_path):
    conn = _get_conn(db_path)
    rows = conn.execute("SELECT * FROM ch_members ORDER BY member_id").fetchall()
    return [dict(r) for r in rows]


def get_holdings(db_path, member_id):
    conn = _get_conn(db_path)
    rows = conn.execute(
        "SELECT * FROM ch_holdings WHERE member_id=? AND quantity>0", (member_id,)
    ).fetchall()
    return [dict(r) for r in rows]


def record_trade(db_path, member_id, symbol, side, quantity, price, cl_ord_id=""):
    conn = _get_conn(db_path)
    trading_date = time.strftime("%Y-%m-%d")
    try:
        conn.execute("BEGIN")

        if side == "Buy":
            cost = quantity * price
            conn.execute(
                "UPDATE ch_members SET capital = capital - ? WHERE member_id=?",
                (cost, member_id)
            )
            row = conn.execute(
                "SELECT quantity, avg_cost FROM ch_holdings WHERE member_id=? AND symbol=?",
                (member_id, symbol)
            ).fetchone()
            if row and row["quantity"] > 0:
                old_qty, old_avg = row["quantity"], row["avg_cost"]
                new_qty = old_qty + quantity
                new_avg = (old_qty * old_avg + quantity * price) / new_qty
                conn.execute(
                    "UPDATE ch_holdings SET quantity=?, avg_cost=? WHERE member_id=? AND symbol=?",
                    (new_qty, new_avg, member_id, symbol)
                )
            else:
                conn.execute(
                    "INSERT OR REPLACE INTO ch_holdings (member_id, symbol, quantity, avg_cost) "
                    "VALUES (?, ?, ?, ?)",
                    (member_id, symbol, quantity, price)
                )
        else:
            row = conn.execute(
                "SELECT quantity, avg_cost FROM ch_holdings WHERE member_id=? AND symbol=?",
                (member_id, symbol)
            ).fetchone()
            if row:
                realized = quantity * (price - row["avg_cost"])
                conn.execute(
                    "UPDATE ch_members SET capital = capital + ? WHERE member_id=?",
                    (quantity * price, member_id)
                )
                new_qty = row["quantity"] - quantity
                if new_qty <= 0:
                    conn.execute(
                        "DELETE FROM ch_holdings WHERE member_id=? AND symbol=?",
                        (member_id, symbol)
                    )
                else:
                    conn.execute(
                        "UPDATE ch_holdings SET quantity=? WHERE member_id=? AND symbol=?",
                        (new_qty, member_id, symbol)
                    )

        conn.execute(
            "INSERT INTO ch_trade_log (member_id, symbol, side, quantity, price, cl_ord_id, "
            "trading_date, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (member_id, symbol, side, quantity, price, cl_ord_id, trading_date, time.time())
        )

        conn.execute("""
            INSERT INTO ch_daily_trades (member_id, trading_date, buy_count, sell_count, total_securities)
            VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(member_id, trading_date) DO UPDATE SET
                buy_count = buy_count + ?,
                sell_count = sell_count + ?,
                total_securities = total_securities + ?
        """, (
            member_id, trading_date,
            quantity if side == "Buy" else 0,
            quantity if side == "Sell" else 0,
            quantity,
            quantity if side == "Buy" else 0,
            quantity if side == "Sell" else 0,
            quantity,
        ))

        conn.execute("COMMIT")
    except Exception:
        conn.execute("ROLLBACK")
        raise


def get_daily_stats(db_path, member_id, trading_date=None):
    conn = _get_conn(db_path)
    if not trading_date:
        trading_date = time.strftime("%Y-%m-%d")
    row = conn.execute(
        "SELECT * FROM ch_daily_trades WHERE member_id=? AND trading_date=?",
        (member_id, trading_date)
    ).fetchone()
    if row:
        return dict(row)
    return {"member_id": member_id, "trading_date": trading_date,
            "buy_count": 0, "sell_count": 0, "total_securities": 0}


def get_trade_log(db_path, member_id, limit=20):
    conn = _get_conn(db_path)
    rows = conn.execute(
        "SELECT * FROM ch_trade_log WHERE member_id=? ORDER BY id DESC LIMIT ?",
        (member_id, limit)
    ).fetchall()
    return [dict(r) for r in rows]


def record_settlement(db_path, member_id, trading_date, opening_capital,
                       closing_capital, realized_pnl, unrealized_pnl, obligation_met):
    conn = _get_conn(db_path)
    conn.execute("""
        INSERT INTO ch_settlements (member_id, trading_date, opening_capital, closing_capital,
                                     realized_pnl, unrealized_pnl, obligation_met)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    """, (member_id, trading_date, opening_capital, closing_capital,
          realized_pnl, unrealized_pnl, 1 if obligation_met else 0))
    conn.commit()


def get_leaderboard(db_path):
    conn = _get_conn(db_path)
    members = conn.execute("SELECT * FROM ch_members ORDER BY capital DESC").fetchall()
    result = []
    trading_date = time.strftime("%Y-%m-%d")
    for m in members:
        m = dict(m)
        holdings = conn.execute(
            "SELECT symbol, quantity, avg_cost FROM ch_holdings WHERE member_id=? AND quantity>0",
            (m["member_id"],)
        ).fetchall()
        m["holdings"] = [dict(h) for h in holdings]

        daily = conn.execute(
            "SELECT buy_count, sell_count, total_securities FROM ch_daily_trades "
            "WHERE member_id=? AND trading_date=?",
            (m["member_id"], trading_date)
        ).fetchone()
        m["daily"] = dict(daily) if daily else {"buy_count": 0, "sell_count": 0, "total_securities": 0}
        m["obligation_met"] = m["daily"]["total_securities"] >= 20
        result.append(m)
    return result
