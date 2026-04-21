"""
EuNEx Clearing House — AI Trading Members.

Rule-based strategies (no external LLM dependencies):
  - momentum: follows recent price trend
  - mean_revert: bets on price reversal toward moving average
  - random: random buy/sell within constraints

Each AI member trades periodically, respecting capital and holdings limits.
"""

import time
import random
import threading
import json
import urllib.request
import urllib.error
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from shared.config import (
    SYMBOLS, CH_MEMBERS, CH_AI_INTERVAL, CH_DEFAULT_STRATEGIES, DASHBOARD_URL,
)
from clearing_house.ch_database import (
    get_member, get_holdings, get_daily_stats, record_trade,
)


class AITrader:
    def __init__(self, db_path):
        self.db_path = db_path
        self.strategies = dict(CH_DEFAULT_STRATEGIES)
        self.human_active = set()
        self.running = False
        self.suspended = False
        self.price_history = {info["name"]: [] for info in SYMBOLS.values()}
        self._seq = 0
        self._thread = None
        self._trade_thread = None

    def start(self):
        self.running = True
        self.suspended = False
        self._thread = threading.Thread(target=self._simulation_loop, daemon=True)
        self._thread.start()
        self._trade_thread = threading.Thread(target=self._trade_consumer_loop, daemon=True)
        self._trade_thread.start()

    def stop(self):
        self.running = False
        self.suspended = False

    def suspend(self):
        self.suspended = True

    def resume(self):
        self.suspended = False

    def set_human_active(self, member_id):
        self.human_active.add(member_id)

    def set_human_inactive(self, member_id):
        self.human_active.discard(member_id)

    def set_strategy(self, member_id, strategy):
        self.strategies[member_id] = strategy

    def _simulation_loop(self):
        while self.running:
            if not self.suspended:
                for mid in CH_MEMBERS:
                    if mid in self.human_active:
                        continue
                    if not self.running:
                        break
                    try:
                        self._trade_for_member(mid)
                    except Exception as e:
                        print(f"[CH-AI] Error for {mid}: {e}")

            for _ in range(CH_AI_INTERVAL * 2):
                if not self.running:
                    return
                time.sleep(0.5)

    def _trade_consumer_loop(self):
        while self.running:
            try:
                resp = urllib.request.urlopen(
                    f"{DASHBOARD_URL}/data", timeout=5
                )
                data = json.loads(resp.read())
                for t in data.get("trades", []):
                    sym = t.get("symbol", "")
                    price = t.get("price", 0)
                    if sym in self.price_history and price > 0:
                        self.price_history[sym].append(price)
                        if len(self.price_history[sym]) > 200:
                            self.price_history[sym] = self.price_history[sym][-100:]
            except Exception:
                pass
            time.sleep(5)

    def _trade_for_member(self, member_id):
        member = get_member(self.db_path, member_id)
        if not member:
            return

        holdings = get_holdings(self.db_path, member_id)
        holdings_map = {h["symbol"]: h for h in holdings}
        bbos = self._get_bbos()
        if not bbos:
            return

        strategy = self.strategies.get(member_id, "random")
        order = None

        if strategy == "momentum":
            order = self._strategy_momentum(member, holdings_map, bbos)
        elif strategy == "mean_revert":
            order = self._strategy_mean_revert(member, holdings_map, bbos)
        else:
            order = self._strategy_random(member, holdings_map, bbos)

        if order:
            self._submit_order(member_id, order)

    def _strategy_momentum(self, member, holdings_map, bbos):
        candidates = []
        for sym, prices in self.price_history.items():
            if len(prices) < 3:
                continue
            recent = prices[-5:]
            trend = recent[-1] - recent[0]
            candidates.append((sym, trend))

        if not candidates:
            return self._strategy_random(member, holdings_map, bbos)

        candidates.sort(key=lambda x: abs(x[1]), reverse=True)
        sym, trend = candidates[0]

        bbo = bbos.get(sym)
        if not bbo:
            return None

        if trend > 0:
            price = bbo.get("bestAsk", 0) or bbo.get("bestBid", 0)
            if price <= 0:
                return None
            max_qty = int(member["capital"] * 0.1 / price)
            qty = max(1, min(max_qty, random.randint(10, 100)))
            if qty * price > member["capital"]:
                return None
            return {"symbol": sym, "side": "Buy", "price": round(price * 1.001, 2), "quantity": qty}
        else:
            held = holdings_map.get(sym)
            if not held or held["quantity"] <= 0:
                return None
            qty = min(held["quantity"], random.randint(10, 50))
            price = bbo.get("bestBid", 0) or bbo.get("bestAsk", 0)
            if price <= 0:
                return None
            return {"symbol": sym, "side": "Sell", "price": round(price * 0.999, 2), "quantity": qty}

    def _strategy_mean_revert(self, member, holdings_map, bbos):
        candidates = []
        for sym, prices in self.price_history.items():
            if len(prices) < 10:
                continue
            avg = sum(prices[-20:]) / len(prices[-20:])
            deviation = (prices[-1] - avg) / avg if avg > 0 else 0
            candidates.append((sym, deviation, avg))

        if not candidates:
            return self._strategy_random(member, holdings_map, bbos)

        candidates.sort(key=lambda x: abs(x[1]), reverse=True)
        sym, deviation, avg = candidates[0]
        bbo = bbos.get(sym)
        if not bbo:
            return None

        if deviation < -0.01:
            price = bbo.get("bestAsk", 0) or avg
            if price <= 0:
                return None
            max_qty = int(member["capital"] * 0.15 / price)
            qty = max(1, min(max_qty, random.randint(10, 80)))
            if qty * price > member["capital"]:
                return None
            return {"symbol": sym, "side": "Buy", "price": round(price, 2), "quantity": qty}
        elif deviation > 0.01:
            held = holdings_map.get(sym)
            if not held or held["quantity"] <= 0:
                return None
            qty = min(held["quantity"], random.randint(10, 60))
            price = bbo.get("bestBid", 0) or avg
            if price <= 0:
                return None
            return {"symbol": sym, "side": "Sell", "price": round(price, 2), "quantity": qty}
        return None

    def _strategy_random(self, member, holdings_map, bbos):
        active_syms = [s for s in bbos if bbos[s].get("bestBid", 0) > 0 or bbos[s].get("bestAsk", 0) > 0]
        if not active_syms:
            sym_names = [info["name"] for info in SYMBOLS.values()]
            if not sym_names:
                return None
            sym = random.choice(sym_names)
            start_price = next(
                (info["startPrice"] for info in SYMBOLS.values() if info["name"] == sym), 100
            )
            side = random.choice(["Buy", "Sell"])
            qty = random.randint(10, 50)
            if side == "Buy":
                if qty * start_price > member["capital"]:
                    qty = max(1, int(member["capital"] * 0.05 / start_price))
                return {"symbol": sym, "side": side, "price": round(start_price, 2), "quantity": qty}
            else:
                held = holdings_map.get(sym)
                if held and held["quantity"] > 0:
                    qty = min(held["quantity"], qty)
                    return {"symbol": sym, "side": side, "price": round(start_price, 2), "quantity": qty}
                if qty * start_price > member["capital"]:
                    qty = max(1, int(member["capital"] * 0.05 / start_price))
                return {"symbol": sym, "side": "Buy", "price": round(start_price, 2), "quantity": qty}

        sym = random.choice(active_syms)
        bbo = bbos[sym]
        side = random.choice(["Buy", "Sell"])
        qty = random.randint(10, 100)

        if side == "Buy":
            price = bbo.get("bestAsk", 0) or bbo.get("bestBid", 0)
            if price <= 0:
                return None
            max_qty = int(member["capital"] * 0.1 / price)
            qty = max(1, min(qty, max_qty))
            if qty * price > member["capital"]:
                return None
            return {"symbol": sym, "side": side, "price": round(price * 1.002, 2), "quantity": qty}
        else:
            held = holdings_map.get(sym)
            if not held or held["quantity"] <= 0:
                price = bbo.get("bestAsk", 0) or bbo.get("bestBid", 0)
                if price <= 0:
                    return None
                max_qty = int(member["capital"] * 0.05 / price)
                qty = max(1, min(qty, max_qty))
                if qty * price > member["capital"]:
                    return None
                return {"symbol": sym, "side": "Buy", "price": round(price, 2), "quantity": qty}
            qty = min(held["quantity"], qty)
            price = bbo.get("bestBid", 0) or bbo.get("bestAsk", 0)
            if price <= 0:
                return None
            return {"symbol": sym, "side": side, "price": round(price * 0.998, 2), "quantity": qty}

    def _submit_order(self, member_id, order):
        self._seq += 1
        cl_ord_id = f"{member_id}-{int(time.time()*1000)}-{self._seq}"

        sym_id = None
        for sid, info in SYMBOLS.items():
            if info["name"] == order["symbol"]:
                sym_id = sid
                break
        if sym_id is None:
            return

        data = {
            "symbolIdx": sym_id,
            "side": order["side"],
            "orderType": "Limit",
            "price": order["price"],
            "quantity": order["quantity"],
            "tif": "Day",
            "source": f"CH-{member_id}",
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
                        record_trade(
                            self.db_path, member_id, order["symbol"],
                            order["side"], fill_qty, order["price"], cl_ord_id,
                        )
                print(f"[CH-AI] {member_id} ({self.strategies.get(member_id, '?')}): "
                      f"{order['side']} {order['quantity']} {order['symbol']} @ {order['price']} "
                      f"→ {result.get('status', '?')}")
        except Exception as e:
            print(f"[CH-AI] Submit failed for {member_id}: {e}")

    def _get_bbos(self):
        try:
            with urllib.request.urlopen(f"{DASHBOARD_URL}/data", timeout=5) as resp:
                data = json.loads(resp.read())
                bbos = {}
                for sid_str, snap in data.get("snapshots", {}).items():
                    bbos[snap.get("symbol", "")] = snap
                return bbos
        except Exception:
            return {}
