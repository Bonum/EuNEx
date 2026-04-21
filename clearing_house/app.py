"""
EuNEx Clearing House — Member portal and AI trading coordinator.

Provides:
  - Member leaderboard with capital, holdings, P&L
  - Member login/portfolio view
  - Order submission for human members
  - AI trading member coordination
  - EOD settlement
  - REST API for dashboard integration

Run: python clearing_house/app.py
"""

import json
import time
import queue
import threading
import sys
import os
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from flask import Flask, render_template, request, jsonify, Response, session, redirect

from shared.config import (
    SYMBOLS, CH_PORT, CH_DB, CH_MEMBERS, CH_DEFAULT_STRATEGIES,
    CH_OBLIGATION_MIN_SECURITIES, DASHBOARD_URL,
)
from clearing_house.ch_database import (
    init_db, get_member, get_all_members, get_holdings,
    get_daily_stats, get_trade_log, get_leaderboard,
    record_trade, record_settlement,
)
from clearing_house.ch_ai_trader import AITrader

app = Flask(__name__)
app.secret_key = "eunex-ch-secret-key"

db_path = CH_DB
ai_trader = AITrader(db_path)

sse_clients = []


def broadcast(event_type, data):
    msg = f"event: {event_type}\ndata: {json.dumps(data, default=str)}\n\n"
    dead = []
    for i, q in enumerate(sse_clients):
        try:
            q.put_nowait(msg)
        except queue.Full:
            dead.append(i)
    for i in reversed(dead):
        sse_clients.pop(i)


# ── Web routes ──────────────────────────────────────────────────────

@app.route("/")
def index():
    leaderboard = get_leaderboard(db_path)
    bbos = _get_bbos()
    for m in leaderboard:
        holdings_value = sum(
            h["quantity"] * bbos.get(h["symbol"], {}).get("lastPrice", h["avg_cost"])
            for h in m.get("holdings", [])
        )
        m["holdings_value"] = round(holdings_value, 2)
        m["total_value"] = round(m["capital"] + holdings_value, 2)
    leaderboard.sort(key=lambda x: x["total_value"], reverse=True)
    return render_template("dashboard.html", members=leaderboard, symbols=SYMBOLS,
                           strategies=ai_trader.strategies)

@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        member_id = request.form.get("member_id", "").upper()
        password = request.form.get("password", "")
        if member_id in CH_MEMBERS and password == member_id.lower():
            session["member_id"] = member_id
            ai_trader.set_human_active(member_id)
            broadcast("member_login", {"member_id": member_id})
            return redirect("/portfolio")
        return render_template("login.html", error="Invalid credentials")
    return render_template("login.html")

@app.route("/logout")
def logout():
    mid = session.pop("member_id", None)
    if mid:
        ai_trader.set_human_inactive(mid)
        broadcast("member_logout", {"member_id": mid})
    return redirect("/")

@app.route("/portfolio")
def portfolio():
    mid = session.get("member_id")
    if not mid:
        return redirect("/login")
    member = get_member(db_path, mid)
    holdings = get_holdings(db_path, mid)
    daily = get_daily_stats(db_path, mid)
    trades = get_trade_log(db_path, mid, 20)
    bbos = _get_bbos()
    return render_template("portfolio.html", member=member, holdings=holdings,
                           daily=daily, trades=trades, symbols=SYMBOLS, bbos=bbos)

@app.route("/order", methods=["POST"])
def submit_order():
    mid = session.get("member_id")
    if not mid:
        return jsonify({"error": "Not logged in"}), 401

    d = request.form
    symbol = d.get("symbol")
    side = d.get("side")
    qty = int(d.get("quantity", 0))
    price = float(d.get("price", 0))

    member = get_member(db_path, mid)
    if side == "Buy" and qty * price > member["capital"]:
        return jsonify({"error": "Insufficient capital"}), 400

    if side == "Sell":
        holdings = get_holdings(db_path, mid)
        held = sum(h["quantity"] for h in holdings if h["symbol"] == symbol)
        if qty > held:
            return jsonify({"error": "Insufficient holdings"}), 400

    sym_id = None
    for sid, info in SYMBOLS.items():
        if info["name"] == symbol:
            sym_id = sid
            break

    cl_ord_id = f"{mid}-{int(time.time()*1000)}-H"
    data = {
        "symbolIdx": sym_id,
        "side": side,
        "orderType": "Limit",
        "price": price,
        "quantity": qty,
        "tif": "Day",
        "source": f"CH-{mid}",
        "clOrdId": cl_ord_id,
    }

    try:
        body = json.dumps(data).encode()
        req = urllib.request.Request(
            f"{DASHBOARD_URL}/order/new",
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=5) as resp:
            result = json.loads(resp.read())
            if result.get("status") in ("Filled", "PartiallyFilled"):
                fill_qty = result["quantity"] - result.get("remainingQty", 0)
                if fill_qty > 0:
                    record_trade(db_path, mid, symbol, side, fill_qty, price, cl_ord_id)
            return redirect("/portfolio")
    except Exception as e:
        return jsonify({"error": str(e)}), 500


# ── API routes ──────────────────────────────────────────────────────

@app.route("/api/leaderboard")
def api_leaderboard():
    leaderboard = get_leaderboard(db_path)
    bbos = _get_bbos()
    for m in leaderboard:
        holdings_value = sum(
            h["quantity"] * bbos.get(h["symbol"], {}).get("lastPrice", h["avg_cost"])
            for h in m.get("holdings", [])
        )
        m["holdings_value"] = round(holdings_value, 2)
        m["total_value"] = round(m["capital"] + holdings_value, 2)
    leaderboard.sort(key=lambda x: x["total_value"], reverse=True)
    return jsonify(leaderboard)

@app.route("/api/member/<member_id>")
def api_member(member_id):
    member = get_member(db_path, member_id)
    if not member:
        return jsonify({"error": "Not found"}), 404
    member["holdings"] = get_holdings(db_path, member_id)
    member["daily"] = get_daily_stats(db_path, member_id)
    member["trades"] = get_trade_log(db_path, member_id, 20)
    member["strategy"] = ai_trader.strategies.get(member_id, "random")
    return jsonify(member)

@app.route("/api/member/<member_id>/strategy", methods=["POST"])
def api_set_strategy(member_id):
    d = request.json
    strategy = d.get("strategy", "random")
    if strategy not in ("momentum", "mean_revert", "random"):
        return jsonify({"error": "Invalid strategy"}), 400
    ai_trader.set_strategy(member_id, strategy)
    return jsonify({"member_id": member_id, "strategy": strategy})

@app.route("/api/control", methods=["POST"])
def api_control():
    d = request.json
    action = d.get("action", "")
    if action == "start":
        ai_trader.start()
    elif action == "stop":
        ai_trader.stop()
    elif action == "suspend":
        ai_trader.suspend()
    elif action == "resume":
        ai_trader.resume()
    return jsonify({"action": action, "status": "ok"})

@app.route("/api/config")
def api_config():
    return jsonify({
        "strategies": ai_trader.strategies,
        "members": list(CH_MEMBERS.keys()),
        "human_active": list(ai_trader.human_active),
        "obligation_min": CH_OBLIGATION_MIN_SECURITIES,
        "ai_interval": ai_trader.running,
    })

@app.route("/api/eod", methods=["POST"])
def api_eod():
    trading_date = time.strftime("%Y-%m-%d")
    bbos = _get_bbos()
    results = []
    for mid in CH_MEMBERS:
        member = get_member(db_path, mid)
        if not member:
            continue
        holdings = get_holdings(db_path, mid)
        daily = get_daily_stats(db_path, mid)

        unrealized = sum(
            h["quantity"] * (bbos.get(h["symbol"], {}).get("lastPrice", h["avg_cost"]) - h["avg_cost"])
            for h in holdings
        )
        realized = member["capital"] - CH_MEMBERS[mid]["capital"]
        obligation_met = daily.get("total_securities", 0) >= CH_OBLIGATION_MIN_SECURITIES

        record_settlement(
            db_path, mid, trading_date,
            CH_MEMBERS[mid]["capital"], member["capital"],
            round(realized, 2), round(unrealized, 2), obligation_met,
        )
        results.append({
            "member_id": mid,
            "closing_capital": member["capital"],
            "realized_pnl": round(realized, 2),
            "unrealized_pnl": round(unrealized, 2),
            "obligation_met": obligation_met,
        })

    broadcast("eod_settlement", {"date": trading_date, "results": results})
    return jsonify(results)

@app.route("/stream")
def sse_stream():
    q = queue.Queue(maxsize=100)
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
                    headers={"Cache-Control": "no-cache"})

@app.route("/health")
def health():
    return jsonify({"status": "ok", "ai_running": ai_trader.running,
                    "members": len(CH_MEMBERS)})


def _get_bbos():
    try:
        with urllib.request.urlopen(f"{DASHBOARD_URL}/data", timeout=3) as resp:
            data = json.loads(resp.read())
            bbos = {}
            for sid_str, snap in data.get("snapshots", {}).items():
                bbos[snap.get("symbol", "")] = snap
            return bbos
    except Exception:
        return {}


if __name__ == "__main__":
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    init_db(db_path, CH_MEMBERS)
    print(f"EuNEx Clearing House starting on http://localhost:{CH_PORT}")
    print(f"  Database: {db_path}")
    print(f"  Members: {len(CH_MEMBERS)}")
    print(f"  Dashboard: {DASHBOARD_URL}")
    app.run(host="0.0.0.0", port=CH_PORT, debug=False, threaded=True)
