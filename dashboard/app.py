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
  - AI Analyst (Ollama/Groq/HuggingFace)
  - Developer Message Flow Visualizer

Run: python dashboard/app.py
"""

import json
import time
import queue
import threading
import random
import sys
import os
import urllib.request
import urllib.error
from collections import deque

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from flask import Flask, render_template, request, jsonify, Response
from dashboard.database import (
    init_db, save_order, save_trade, record_ohlcv,
    get_ohlcv, get_recent_orders, get_recent_trades, get_active_orders,
    save_daily_close, get_last_closing_prices, get_daily_closes,
)
from datetime import date
from shared.config import SYMBOLS, DASHBOARD_PORT, DASHBOARD_DB, CH_URL, SIM_INTERVAL, SIM_ORDERS_PER_ROUND

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

@app.route("/history/daily/<symbol>")
def history_daily(symbol):
    limit = int(request.args.get("limit", 30))
    rows = get_daily_closes(db_path, symbol, limit)
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
    _seed_initial_orders()
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
    _save_closing_prices()
    session_status = "idle"
    broadcast_event("session", {"status": session_status})
    _notify_ch("stop")
    return jsonify({"status": session_status})

@app.route("/session/status")
def session_status_route():
    return jsonify({"status": session_status})


def _seed_initial_orders():
    prev_closes = get_last_closing_prices(db_path)
    for sym_id, info in symbols.items():
        sym_name = info["name"]
        if sym_name in prev_closes:
            ref = prev_closes[sym_name]["close_price"]
        else:
            ref = info["startPrice"]
        for offset, qty in [(-0.50, 150), (-1.00, 100), (0.50, 200), (1.00, 100)]:
            side = "Buy" if offset < 0 else "Sell"
            engine.submit_order(
                symbol_id=sym_id, side=side, order_type="Limit",
                price=round(ref + offset, 2), quantity=qty,
                tif="Day", source="seed",
            )


def _save_closing_prices():
    today = date.today().isoformat()
    with state_lock:
        for sym_id, info in symbols.items():
            snap = snapshots.get(sym_id, {})
            sym_trades = [t for t in trades if t["symbolIdx"] == sym_id]
            close_price = snap.get("lastPrice", 0) or info["startPrice"]
            bid = snap.get("bestBid", 0)
            ask = snap.get("bestAsk", 0)
            volume = sum(t["quantity"] for t in sym_trades)
            trade_count = len(sym_trades)
            save_daily_close(db_path, info["name"], today,
                             close_price, bid, ask, volume, trade_count)


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


# ── Market Simulation ──────────────────────────────────────────────

class MarketSimulator:
    def __init__(self, engine_ref, interval=SIM_INTERVAL, orders_per_round=SIM_ORDERS_PER_ROUND):
        self.engine = engine_ref
        self.interval = interval
        self.orders_per_round = orders_per_round
        self._thread = None
        self._running = False
        self._ref_prices = {sid: info["startPrice"] for sid, info in symbols.items()}
        self._load_closing_refs()

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False

    def _run(self):
        while self._running:
            if session_status == "active":
                self._round()
            time.sleep(self.interval)

    def _round(self):
        sym_ids = list(symbols.keys())
        random.shuffle(sym_ids)

        for sym_id in sym_ids:
            ref = self._current_ref(sym_id)
            for _ in range(self.orders_per_round):
                side = random.choice(["Buy", "Sell"])
                spread_pct = random.uniform(-0.005, 0.005)
                price = round(ref * (1 + spread_pct), 2)
                if price <= 0:
                    price = 0.01
                qty = random.randint(10, 200)

                self.engine.submit_order(
                    symbol_id=sym_id,
                    side=side,
                    order_type="Limit",
                    price=price,
                    quantity=qty,
                    tif="Day",
                    source="sim",
                )

            cross_side = random.choice(["Buy", "Sell"])
            snap = snapshots.get(sym_id, {})
            if cross_side == "Buy" and snap.get("bestAsk", 0) > 0:
                cross_price = snap["bestAsk"] + 0.01
            elif cross_side == "Sell" and snap.get("bestBid", 0) > 0:
                cross_price = snap["bestBid"] - 0.01
            else:
                cross_price = ref
            if cross_price <= 0:
                cross_price = 0.01
            cross_qty = random.randint(10, 50)

            self.engine.submit_order(
                symbol_id=sym_id,
                side=cross_side,
                order_type="Limit",
                price=round(cross_price, 2),
                quantity=cross_qty,
                tif="Day",
                source="sim",
            )

    def _load_closing_refs(self):
        try:
            prev = get_last_closing_prices(db_path)
            for sid, info in symbols.items():
                if info["name"] in prev:
                    self._ref_prices[sid] = prev[info["name"]]["close_price"]
        except Exception:
            pass

    def _current_ref(self, sym_id):
        snap = snapshots.get(sym_id, {})
        bid = snap.get("bestBid", 0)
        ask = snap.get("bestAsk", 0)
        if bid > 0 and ask > 0:
            return (bid + ask) / 2
        if snap.get("lastPrice", 0) > 0:
            return snap["lastPrice"]
        return self._ref_prices.get(sym_id, 100.0)


simulator = MarketSimulator(engine)


# ── AI Analyst ─────────────────────────────────────────────────────

OLLAMA_HOST = os.environ.get("OLLAMA_HOST", "http://localhost:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "llama3.2:3b")
GROQ_API_KEY = os.environ.get("GROQ_API_KEY", "")
GROQ_MODEL = os.environ.get("GROQ_MODEL", "llama-3.1-8b-instant")
HF_TOKEN = os.environ.get("HF_TOKEN", "")
HF_MODEL = os.environ.get("HF_MODEL", "Qwen/Qwen2.5-7B-Instruct")

ai_insights = deque(maxlen=20)
ai_provider = "auto"
ai_model_override = None
ai_generating = False


def _build_market_prompt():
    now_str = time.strftime("%H:%M:%S")
    session = session_status.upper()

    trade_lines = []
    with state_lock:
        by_sym = {}
        for t in trades[-200:]:
            sym = t.get("symbol", "?")
            by_sym.setdefault(sym, []).append(t)
        for sym, ts in sorted(by_sym.items()):
            prices = [t["price"] for t in ts if t.get("price")]
            vol = sum(t.get("quantity", 0) for t in ts)
            if prices:
                trade_lines.append(
                    f"  {sym}: {len(ts)} trade(s), range {min(prices):.2f}-{max(prices):.2f}, "
                    f"vol {vol}, last {prices[-1]:.2f}"
                )

        book_lines = []
        for sid, snap in sorted(snapshots.items()):
            bid = snap.get("bestBid", 0)
            ask = snap.get("bestAsk", 0)
            spread = ask - bid if bid > 0 and ask > 0 else 0
            book_lines.append(
                f"  {snap.get('symbol','?')}: Bid {bid:.2f} / Ask {ask:.2f} (spread {spread:.2f})"
            )

    trades_text = "\n".join(trade_lines) if trade_lines else "  No recent trades"
    book_text = "\n".join(book_lines) if book_lines else "  No order book data"

    return (
        "You are a concise financial market analyst for the EuNEx simulated exchange "
        "(Euronext Optiq architecture). "
        f"Time: {now_str} | Session: {session}\n\n"
        f"Recent trades:\n{trades_text}\n\n"
        f"Order book:\n{book_text}\n\n"
        "In 3-4 sentences: activity level, notable price moves, market sentiment. "
        "Plain prose, no headers, no bullet points."
    )


def _try_ollama(prompt, model=None):
    model = model or OLLAMA_MODEL
    try:
        data = json.dumps({
            "model": model,
            "messages": [{"role": "user", "content": prompt}],
            "stream": False,
        }).encode()
        req = urllib.request.Request(
            f"{OLLAMA_HOST}/api/chat",
            data=data,
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=90) as resp:
            result = json.loads(resp.read())
            return result.get("message", {}).get("content", "")
    except Exception:
        return None


def _try_groq(prompt, model=None):
    if not GROQ_API_KEY:
        return None
    model = model or GROQ_MODEL
    try:
        data = json.dumps({
            "model": model,
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 300,
            "temperature": 0.7,
        }).encode()
        req = urllib.request.Request(
            "https://api.groq.com/openai/v1/chat/completions",
            data=data,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {GROQ_API_KEY}",
            },
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            result = json.loads(resp.read())
            return result["choices"][0]["message"]["content"]
    except Exception:
        return None


def _try_hf(prompt, model=None):
    if not HF_TOKEN:
        return None
    model = model or HF_MODEL
    try:
        data = json.dumps({
            "model": model,
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 300,
            "temperature": 0.7,
        }).encode()
        req = urllib.request.Request(
            "https://router.huggingface.co/v1/chat/completions",
            data=data,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {HF_TOKEN}",
            },
        )
        with urllib.request.urlopen(req, timeout=90) as resp:
            result = json.loads(resp.read())
            return result["choices"][0]["message"]["content"]
    except Exception:
        return None


def _call_llm(prompt):
    provider = ai_provider
    model = ai_model_override

    if provider == "ollama":
        return _try_ollama(prompt, model), "ollama"
    elif provider == "groq":
        return _try_groq(prompt, model), "groq"
    elif provider == "hf":
        return _try_hf(prompt, model), "hf"

    for name, func in [("ollama", _try_ollama), ("groq", _try_groq), ("hf", _try_hf)]:
        text = func(prompt, model)
        if text:
            return text, name
    return None, None


def _generate_insight():
    global ai_generating
    if ai_generating:
        return
    ai_generating = True
    try:
        prompt = _build_market_prompt()
        text, source = _call_llm(prompt)
        if text:
            insight = {
                "text": text.strip(),
                "timestamp": time.time(),
                "source": source or "unknown",
            }
            ai_insights.append(insight)
            broadcast_event("ai_insight", insight)
    finally:
        ai_generating = False


@app.route("/ai/generate", methods=["POST"])
def ai_generate():
    threading.Thread(target=_generate_insight, daemon=True).start()
    return jsonify({"status": "generating"})


@app.route("/ai/insights")
def ai_insights_list():
    return jsonify(list(ai_insights))


@app.route("/ai/config")
def ai_config():
    return jsonify({
        "provider": ai_provider,
        "model": ai_model_override,
        "providers": {
            "auto": {"label": "Auto (fallback)", "available": True},
            "ollama": {"label": f"Ollama ({OLLAMA_MODEL})", "available": bool(OLLAMA_HOST)},
            "groq": {"label": f"Groq ({GROQ_MODEL})", "available": bool(GROQ_API_KEY)},
            "hf": {"label": f"HuggingFace ({HF_MODEL})", "available": bool(HF_TOKEN)},
        },
    })


@app.route("/ai/select", methods=["POST"])
def ai_select():
    global ai_provider, ai_model_override
    d = request.json
    ai_provider = d.get("provider", "auto")
    ai_model_override = d.get("model") or None
    return jsonify({"provider": ai_provider, "model": ai_model_override})


# ── Developer Message Flow Log ─────────────────────────────────────

message_log = deque(maxlen=500)


def log_message(stage, detail):
    entry = {
        "timestamp": time.time(),
        "stage": stage,
        "detail": detail,
    }
    message_log.append(entry)
    broadcast_event("msgflow", entry)


@app.route("/dev/messages")
def dev_messages():
    limit = int(request.args.get("limit", 100))
    items = list(message_log)[-limit:]
    return jsonify(items)


# Patch engine to log message flow
_orig_submit = engine.submit_order

def _traced_submit(symbol_id, side, order_type, price, quantity,
                   tif="Day", source="dashboard", cl_ord_id=""):
    sym_name = symbols.get(symbol_id, {}).get("name", "?")
    log_message("OEG", f"NewOrder {sym_name} {side} {quantity}@{price:.2f} [{source}]")
    result = _orig_submit(symbol_id, side, order_type, price, quantity, tif, source, cl_ord_id)
    oid = result.get("orderId", "?")
    status = result.get("status", "?")
    log_message("Book", f"Order#{oid} → {status}")
    if status == "Filled":
        log_message("Match", f"Order#{oid} fully filled")
    elif status == "PartiallyFilled":
        log_message("Match", f"Order#{oid} partial fill, rem={result.get('remainingQty', '?')}")
    return result

engine.submit_order = _traced_submit

_orig_cancel = engine.cancel_order

def _traced_cancel(order_id):
    log_message("OEG", f"CancelOrder #{order_id}")
    result = _orig_cancel(order_id)
    if result:
        log_message("Book", f"Order#{order_id} cancelled")
    return result

engine.cancel_order = _traced_cancel

# Patch trade saving to log trade and clearing steps
_orig_broadcast = broadcast_event

def _traced_broadcast(event_type, data):
    if event_type == "trade":
        tid = data.get("tradeId", "?")
        sym = data.get("symbol", "?")
        log_message("Trade", f"Trade#{tid} {sym} {data.get('quantity',0)}@{data.get('price',0):.2f}")
        log_message("DB", f"Trade#{tid} persisted to SQLite")
        log_message("CH", f"Trade#{tid} → clearing (buy#{data.get('buyOrderId','?')}, sell#{data.get('sellOrderId','?')})")
    _orig_broadcast(event_type, data)

broadcast_event = _traced_broadcast


if __name__ == "__main__":
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    init_db(db_path)
    simulator.start()
    port = DASHBOARD_PORT
    print(f"EuNEx Dashboard starting on http://localhost:{port}")
    print(f"  Database: {db_path}")
    print(f"  Simulation: every {SIM_INTERVAL}s, {SIM_ORDERS_PER_ROUND} orders/symbol/round")
    print(f"  AI Analyst: provider={ai_provider} (Ollama={OLLAMA_HOST})")
    app.run(host="0.0.0.0", port=port, debug=True, threaded=True)
