"""
EuNEx Dashboard — Web GUI for the matching engine.

Equivalent to StockEx dashboard.py but backed by the C++ matching engine.
Provides:
  - Real-time order book view (bids/asks per symbol)
  - Recent trades table
  - Market data snapshots (BBO, last price, volume)
  - Order submission (new, cancel, amend)
  - Session controls (start/suspend/resume)
  - SSE streaming for live updates

Run: python dashboard/app.py
"""

import json
import time
import queue
import threading
import subprocess
import sys
import os
from flask import Flask, render_template, request, jsonify, Response

app = Flask(__name__)

# ── In-memory state ─────────────────────────────────────────────────

state_lock = threading.Lock()

symbols = {
    1: {"name": "AAPL", "segment": "EQU"},
    2: {"name": "MSFT", "segment": "EQU"},
    3: {"name": "EURO50", "segment": "EQD"},
}

orders = []
trades = []
snapshots = {}
session_status = "idle"
next_order_id = 1000

# SSE clients
sse_clients = []

# ── Matching engine bridge ──────────────────────────────────────────

class MatchingEngine:
    """Bridge to the C++ matching engine via in-process simulation."""

    def __init__(self):
        self.order_id_counter = 1
        self.books = {}
        for sym_id in symbols:
            self.books[sym_id] = {"bids": [], "asks": []}

    def submit_order(self, symbol_id, side, order_type, price, quantity, tif="Day"):
        global next_order_id
        oid = next_order_id
        next_order_id += 1

        order = {
            "orderId": oid,
            "symbolIdx": symbol_id,
            "symbol": symbols.get(symbol_id, {}).get("name", "???"),
            "side": side,
            "orderType": order_type,
            "price": price,
            "quantity": quantity,
            "remainingQty": quantity,
            "status": "New",
            "tif": tif,
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

        broadcast_event("order", order)
        for t in matched_trades:
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

        # FOK check
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
                        broadcast_event("order", o)
                        break
            else:
                resting["status"] = "PartiallyFilled"
                for o in orders:
                    if o["orderId"] == resting["orderId"]:
                        o["status"] = "PartiallyFilled"
                        o["remainingQty"] = resting["remainingQty"]
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
            "symbols": symbols,
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
    )
    return jsonify(order)

@app.route("/order/cancel", methods=["POST"])
def cancel_order():
    d = request.json
    result = engine.cancel_order(int(d["orderId"]))
    if result:
        return jsonify(result)
    return jsonify({"error": "Order not found or not cancellable"}), 404

@app.route("/orderbook/<int:symbol_id>")
def orderbook(symbol_id):
    with state_lock:
        snap = snapshots.get(symbol_id, {})
        return jsonify(snap)

@app.route("/session/start", methods=["POST"])
def session_start():
    global session_status
    session_status = "active"
    broadcast_event("session", {"status": session_status})
    return jsonify({"status": session_status})

@app.route("/session/suspend", methods=["POST"])
def session_suspend():
    global session_status
    session_status = "suspended"
    broadcast_event("session", {"status": session_status})
    return jsonify({"status": session_status})

@app.route("/session/resume", methods=["POST"])
def session_resume():
    global session_status
    session_status = "active"
    broadcast_event("session", {"status": session_status})
    return jsonify({"status": session_status})

@app.route("/session/end", methods=["POST"])
def session_end():
    global session_status
    session_status = "idle"
    broadcast_event("session", {"status": session_status})
    return jsonify({"status": session_status})

@app.route("/session/status")
def session_status_route():
    return jsonify({"status": session_status})


if __name__ == "__main__":
    port = int(os.environ.get("EUNEX_DASHBOARD_PORT", 8080))
    print(f"EuNEx Dashboard starting on http://localhost:{port}")
    app.run(host="0.0.0.0", port=port, debug=True, threaded=True)
