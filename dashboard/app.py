"""
EuNEx Dashboard — Web GUI for the matching engine.

Provides:
  - Real-time order book view (bids/asks per symbol)
  - Recent trades table with SQLite persistence
  - Market data snapshots (BBO, last price, volume)
  - Order submission (new, cancel, amend)
  - OHLCV chart data (1h, 8h, 1d, 1w)
  - Session controls (start/suspend/resume)
  - SSE streaming for live updates
  - Clearing House integration

Run: python dashboard/app.py
"""

import json
import time
import queue
import threading
import sys
import os
import urllib.request
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from flask import Flask, render_template, request, jsonify, Response
from dashboard.database import (
    init_db, save_order, save_trade, record_ohlcv,
    get_ohlcv, get_recent_orders, get_recent_trades, get_active_orders,
)
from shared.config import SYMBOLS, DASHBOARD_PORT, DASHBOARD_DB, CH_URL

app = Flask(__name__)

# ── In-memory state ─────────────────────────────────────────────────

state_lock = threading.Lock()

symbols = SYMBOLS
orders = []
trades = []
snapshots = {}
session_status = "idle"
next_order_id = 1000
db_path = DASHBOARD_DB

sse_clients = []

# ── Matching engine bridge ──────────────────────────────────────────

class MatchingEngine:
    def __init__(self):
        self.books = {}
        for sym_id in symbols:
            self.books[sym_id] = {"bids": [], "asks": []}

    def submit_order(self, symbol_id, side, order_type, price, quantity,
                     tif="Day", source="dashboard", cl_ord_id=""):
        global next_order_id
        oid = next_order_id
        next_order_id += 1

        order = {
            "orderId": oid,
            "clOrdId": cl_ord_id or f"D-{oid}",
            "symbolIdx": symbol_id,
            "symbol": symbols.get(symbol_id, {}).get("name", "???"),
            "side": side,
            "orderType": order_type,
            "price": price,
            "quantity": quantity,
            "remainingQty": quantity,
            "status": "New",
            "tif": tif,
            "source": source,
            "timestamp": time.time(),
        }

        with state_lock:
            matched_trades = self._match(order)
            if order["remainingQty"] > 0 and order["status"] not in ("Cancelled", "Rejected"):
                if order_type == "Market" or tif == "IOC":
                    order["status"] = "Cancelled"
                elif tif == "FOK":
                    order["status"] = "Rejected"
                else:
                    orders.append(order)
                    self._insert_resting(order)
            elif order["remainingQty"] == 0:
                order["status"] = "Filled"

            self._update_snapshot(symbol_id)

        save_order(db_path, order)
        broadcast_event("order", order)
        for t in matched_trades:
            save_trade(db_path, t)
            record_ohlcv(db_path, t["symbol"], t["price"], t["quantity"])
            broadcast_event("trade", t)
        broadcast_event("snapshot", snapshots.get(symbol_id, {}))

        return order

    def cancel_order(self, order_id):
        with state_lock:
            for o in orders:
                if o["orderId"] == order_id and o["status"] in ("New", "PartiallyFilled"):
                    o["status"] = "Cancelled"
                    self._remove_from_book(o)
                    self._update_snapshot(o["symbolIdx"])
                    save_order(db_path, o)
                    broadcast_event("order", o)
                    return o
        return None

    def amend_order(self, order_id, new_price=None, new_quantity=None):
        with state_lock:
            for o in orders:
                if o["orderId"] == order_id and o["status"] in ("New", "PartiallyFilled"):
                    self._remove_from_book(o)
                    if new_price is not None:
                        o["price"] = new_price
                    if new_quantity is not None:
                        o["quantity"] = new_quantity
                        o["remainingQty"] = new_quantity
                    self._insert_resting(o)
                    self._update_snapshot(o["symbolIdx"])
                    save_order(db_path, o)
                    broadcast_event("order", o)
                    return o
        return None

    def _match(self, incoming):
        matched = []
        sym = incoming["symbolIdx"]
        book = self.books[sym]

        if incoming["side"] == "Buy":
            opposite = book["asks"]
        else:
            opposite = book["bids"]

        if incoming["tif"] == "FOK":
            available = 0
            for level in opposite:
                if incoming["side"] == "Buy":
                    if incoming["orderType"] == "Market" or level["price"] <= incoming["price"]:
                        available += level["remainingQty"]
                else:
                    if incoming["orderType"] == "Market" or level["price"] >= incoming["price"]:
                        available += level["remainingQty"]
            if available < incoming["quantity"]:
                incoming["status"] = "Rejected"
                return []

        i = 0
        while i < len(opposite) and incoming["remainingQty"] > 0:
            resting = opposite[i]

            price_ok = incoming["orderType"] == "Market"
            if not price_ok:
                if incoming["side"] == "Buy":
                    price_ok = resting["price"] <= incoming["price"]
                else:
                    price_ok = resting["price"] >= incoming["price"]

            if not price_ok:
                break

            fill_qty = min(incoming["remainingQty"], resting["remainingQty"])
            fill_price = resting["price"]

            incoming["remainingQty"] -= fill_qty
            resting["remainingQty"] -= fill_qty

            if incoming["remainingQty"] > 0:
                incoming["status"] = "PartiallyFilled"
            else:
                incoming["status"] = "Filled"

            if resting["remainingQty"] == 0:
                resting["status"] = "Filled"
                opposite.pop(i)
                for o in orders:
                    if o["orderId"] == resting["orderId"]:
                        o["status"] = "Filled"
                        o["remainingQty"] = 0
                        save_order(db_path, o)
                        broadcast_event("order", o)
                        break
            else:
                resting["status"] = "PartiallyFilled"
                for o in orders:
                    if o["orderId"] == resting["orderId"]:
                        o["status"] = "PartiallyFilled"
                        o["remainingQty"] = resting["remainingQty"]
                        save_order(db_path, o)
                        broadcast_event("order", o)
                        break
                i += 1

            trade = {
                "tradeId": len(trades) + 1,
                "symbolIdx": sym,
                "symbol": symbols.get(sym, {}).get("name", "???"),
                "price": fill_price,
                "quantity": fill_qty,
                "buyOrderId": incoming["orderId"] if incoming["side"] == "Buy" else resting["orderId"],
                "sellOrderId": resting["orderId"] if incoming["side"] == "Buy" else incoming["orderId"],
                "timestamp": time.time(),
            }
            trades.append(trade)
            matched.append(trade)

        return matched

    def _insert_resting(self, order):
        sym = order["symbolIdx"]
        book = self.books[sym]
        entry = {"orderId": order["orderId"], "price": order["price"],
                 "remainingQty": order["remainingQty"], "side": order["side"]}
        if order["side"] == "Buy":
            book["bids"].append(entry)
            book["bids"].sort(key=lambda x: -x["price"])
        else:
            book["asks"].append(entry)
            book["asks"].sort(key=lambda x: x["price"])

    def _remove_from_book(self, order):
        sym = order["symbolIdx"]
        book = self.books[sym]
        side_list = book["bids"] if order["side"] == "Buy" else book["asks"]
        for i, e in enumerate(side_list):
            if e["orderId"] == order["orderId"]:
                side_list.pop(i)
                break

    def _update_snapshot(self, sym_id):
        book = self.books[sym_id]
        sym_trades = [t for t in trades if t["symbolIdx"] == sym_id]

        snap = {
            "symbolIdx": sym_id,
            "symbol": symbols.get(sym_id, {}).get("name", "???"),
            "segment": symbols.get(sym_id, {}).get("segment", ""),
            "bestBid": book["bids"][0]["price"] if book["bids"] else 0,
            "bestAsk": book["asks"][0]["price"] if book["asks"] else 0,
            "bidDepth": len(book["bids"]),
            "askDepth": len(book["asks"]),
            "lastPrice": sym_trades[-1]["price"] if sym_trades else 0,
            "lastQty": sym_trades[-1]["quantity"] if sym_trades else 0,
            "tradeCount": len(sym_trades),
            "volume": sum(t["quantity"] for t in sym_trades),
            "bids": [{"price": b["price"], "qty": b["remainingQty"]} for b in book["bids"][:10]],
            "asks": [{"price": a["price"], "qty": a["remainingQty"]} for a in book["asks"][:10]],
        }
        snapshots[sym_id] = snap


engine = MatchingEngine()


# ── SSE broadcasting ────────────────────────────────────────────────

def broadcast_event(event_type, data):
    msg = f"event: {event_type}\ndata: {json.dumps(data, default=str)}\n\n"
    dead = []
    for i, q in enumerate(sse_clients):
        try:
            q.put_nowait(msg)
        except queue.Full:
            dead.append(i)
    for i in reversed(dead):
        sse_clients.pop(i)


# ── Routes ──────────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/health")
def health():
    return jsonify({"status": "ok", "session": session_status,
                    "orders": len(orders), "trades": len(trades)})

@app.route("/data")
def data():
    with state_lock:
        return jsonify({
            "orders": orders[-50:],
            "trades": trades[-100:],
            "snapshots": {k: v for k, v in snapshots.items()},
            "symbols": {k: v for k, v in symbols.items()},
            "session": session_status,
        })

@app.route("/stream")
def stream():
    q = queue.Queue(maxsize=200)
    sse_clients.append(q)

    def generate():
        try:
            while True:
                try:
                    msg = q.get(timeout=30)
                    yield msg
                except queue.Empty:
                    yield "event: ping\ndata: {}\n\n"
        except GeneratorExit:
            if q in sse_clients:
                sse_clients.remove(q)

    return Response(generate(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache",
                             "X-Accel-Buffering": "no"})

@app.route("/order/new", methods=["POST"])
def new_order():
    d = request.json
    order = engine.submit_order(
        symbol_id=int(d["symbolIdx"]),
        side=d["side"],
        order_type=d.get("orderType", "Limit"),
        price=float(d.get("price", 0)),
        quantity=int(d["quantity"]),
        tif=d.get("tif", "Day"),
        source=d.get("source", "dashboard"),
        cl_ord_id=d.get("clOrdId", ""),
    )
    return jsonify(order)

@app.route("/order/cancel", methods=["POST"])
def cancel_order():
    d = request.json
    result = engine.cancel_order(int(d["orderId"]))
    if result:
        return jsonify(result)
    return jsonify({"error": "Order not found or not cancellable"}), 404

@app.route("/order/amend", methods=["POST"])
def amend_order():
    d = request.json
    result = engine.amend_order(
        order_id=int(d["orderId"]),
        new_price=float(d["price"]) if "price" in d else None,
        new_quantity=int(d["quantity"]) if "quantity" in d else None,
    )
    if result:
        return jsonify(result)
    return jsonify({"error": "Order not found or not amendable"}), 404

@app.route("/orderbook/<int:symbol_id>")
def orderbook(symbol_id):
    with state_lock:
        snap = snapshots.get(symbol_id, {})
        return jsonify(snap)

@app.route("/chart/<symbol>")
def chart_data(symbol):
    period = request.args.get("period", "1h")
    period_map = {"1h": 3600, "8h": 28800, "1d": 86400, "1w": 604800}
    seconds = period_map.get(period, 3600)
    bars = get_ohlcv(db_path, symbol, seconds)
    return jsonify(bars)

@app.route("/history/orders")
def history_orders():
    limit = int(request.args.get("limit", 100))
    rows = get_recent_orders(db_path, limit)
    return jsonify(rows)

@app.route("/history/trades")
def history_trades():
    limit = int(request.args.get("limit", 200))
    rows = get_recent_trades(db_path, limit)
    return jsonify(rows)

@app.route("/ch/leaderboard")
def ch_leaderboard():
    try:
        req = urllib.request.Request(f"{CH_URL}/api/leaderboard")
        with urllib.request.urlopen(req, timeout=3) as resp:
            return jsonify(json.loads(resp.read()))
    except Exception:
        return jsonify([])

# ── Session controls ────────────────────────────────────────────────

@app.route("/session/start", methods=["POST"])
def session_start():
    global session_status
    session_status = "active"
    broadcast_event("session", {"status": session_status})
    _notify_ch("start")
    return jsonify({"status": session_status})

@app.route("/session/suspend", methods=["POST"])
def session_suspend():
    global session_status
    session_status = "suspended"
    broadcast_event("session", {"status": session_status})
    _notify_ch("suspend")
    return jsonify({"status": session_status})

@app.route("/session/resume", methods=["POST"])
def session_resume():
    global session_status
    session_status = "active"
    broadcast_event("session", {"status": session_status})
    _notify_ch("resume")
    return jsonify({"status": session_status})

@app.route("/session/end", methods=["POST"])
def session_end():
    global session_status
    session_status = "idle"
    broadcast_event("session", {"status": session_status})
    _notify_ch("stop")
    return jsonify({"status": session_status})

@app.route("/session/status")
def session_status_route():
    return jsonify({"status": session_status})


def _notify_ch(action):
    try:
        data = json.dumps({"action": action}).encode()
        req = urllib.request.Request(
            f"{CH_URL}/api/control",
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        urllib.request.urlopen(req, timeout=2)
    except Exception:
        pass


if __name__ == "__main__":
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    init_db(db_path)
    port = DASHBOARD_PORT
    print(f"EuNEx Dashboard starting on http://localhost:{port}")
    print(f"  Database: {db_path}")
    app.run(host="0.0.0.0", port=port, debug=True, threaded=True)
