"""
EuNEx FIX 4.4 Gateway — Pure Python TCP acceptor.

Implements a simplified FIX 4.4 protocol:
  - Logon (35=A), Logout (35=5), Heartbeat (35=0)
  - NewOrderSingle (35=D), OrderCancelRequest (35=F), OrderCancelReplaceRequest (35=G)
  - ExecutionReport (35=8) responses

Submits orders to the Dashboard matching engine via HTTP REST.

Usage: python fix_gateway/fix_server.py
"""

import socket
import threading
import time
import json
import sys
import os
import urllib.request
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from shared.config import FIX_PORT, DASHBOARD_URL

SOH = "\x01"
FIX_VERSION = "FIX.4.4"


def parse_fix(raw):
    fields = {}
    for pair in raw.split(SOH):
        if "=" in pair:
            tag, val = pair.split("=", 1)
            fields[int(tag)] = val
    return fields


def build_fix(fields):
    body_parts = []
    for tag, val in fields.items():
        if tag not in (8, 9, 10):
            body_parts.append(f"{tag}={val}")
    body = SOH.join(body_parts)
    header = f"8={FIX_VERSION}{SOH}9={len(body) + 1}{SOH}"
    msg = header + body + SOH
    checksum = sum(ord(c) for c in msg) % 256
    return msg + f"10={checksum:03d}{SOH}"


class FIXSession:
    def __init__(self, conn, addr, server):
        self.conn = conn
        self.addr = addr
        self.server = server
        self.sender_comp_id = "EUNEX"
        self.target_comp_id = ""
        self.seq_num = 1
        self.logged_in = False
        self.running = True
        self.tracked_orders = {}

    def next_seq(self):
        s = self.seq_num
        self.seq_num += 1
        return s

    def send(self, msg_type, fields):
        fields[8] = FIX_VERSION
        fields[35] = msg_type
        fields[49] = self.sender_comp_id
        fields[56] = self.target_comp_id
        fields[34] = self.next_seq()
        fields[52] = time.strftime("%Y%m%d-%H:%M:%S")
        raw = build_fix(fields)
        try:
            self.conn.sendall(raw.encode("ascii"))
        except Exception:
            self.running = False

    def handle(self):
        buf = ""
        while self.running:
            try:
                data = self.conn.recv(4096)
                if not data:
                    break
                buf += data.decode("ascii", errors="replace")

                while "10=" in buf and SOH in buf[buf.index("10="):]:
                    end = buf.index("10=")
                    end = buf.index(SOH, end) + 1
                    msg_raw = buf[:end]
                    buf = buf[end:]
                    self._process(parse_fix(msg_raw))
            except socket.timeout:
                continue
            except Exception as e:
                print(f"[FIX] {self.addr} error: {e}")
                break

        self.running = False
        self.conn.close()
        self.server.remove_session(self)
        print(f"[FIX] {self.addr} disconnected")

    def _process(self, fields):
        msg_type = fields.get(35, "")

        if msg_type == "A":
            self.target_comp_id = fields.get(49, "CLIENT")
            self.logged_in = True
            self.send("A", {98: 0, 108: 30})
            print(f"[FIX] Logon from {self.target_comp_id} @ {self.addr}")

        elif msg_type == "5":
            self.send("5", {})
            self.logged_in = False
            self.running = False
            print(f"[FIX] Logout from {self.target_comp_id}")

        elif msg_type == "0":
            self.send("0", {112: fields.get(112, "")})

        elif msg_type == "1":
            self.send("0", {112: fields.get(112, "TEST")})

        elif msg_type == "D":
            self._handle_new_order(fields)

        elif msg_type == "F":
            self._handle_cancel(fields)

        elif msg_type == "G":
            self._handle_amend(fields)

    def _handle_new_order(self, fields):
        cl_ord_id = fields.get(11, "")
        symbol = fields.get(55, "")
        side_fix = fields.get(54, "1")
        qty = int(fields.get(38, 0))
        price = float(fields.get(44, 0))
        ord_type = fields.get(40, "2")
        tif_fix = fields.get(59, "0")

        side = "Buy" if side_fix == "1" else "Sell"
        order_type = "Market" if ord_type == "1" else "Limit"
        tif_map = {"0": "Day", "1": "GTC", "3": "IOC", "4": "FOK"}
        tif = tif_map.get(tif_fix, "Day")

        sym_id = self._resolve_symbol(symbol)
        if sym_id is None:
            self._send_reject(cl_ord_id, symbol, side_fix, qty, "Unknown symbol")
            return

        order_data = {
            "symbolIdx": sym_id,
            "side": side,
            "orderType": order_type,
            "price": price,
            "quantity": qty,
            "tif": tif,
            "source": f"FIX-{self.target_comp_id}",
            "clOrdId": cl_ord_id,
        }

        result = self._post_order("/order/new", order_data)
        if result:
            self.tracked_orders[cl_ord_id] = result
            self._send_exec_report(
                cl_ord_id=cl_ord_id,
                order_id=str(result.get("orderId", "")),
                exec_type="0",
                ord_status=self._map_status(result.get("status", "New")),
                symbol=symbol,
                side=side_fix,
                qty=qty,
                price=price,
                last_qty=0,
                last_px=0,
                leaves_qty=result.get("remainingQty", qty),
                cum_qty=qty - result.get("remainingQty", qty),
            )

            if result.get("status") in ("Filled", "PartiallyFilled"):
                fill_qty = qty - result.get("remainingQty", qty)
                self._send_exec_report(
                    cl_ord_id=cl_ord_id,
                    order_id=str(result.get("orderId", "")),
                    exec_type="F",
                    ord_status=self._map_status(result["status"]),
                    symbol=symbol,
                    side=side_fix,
                    qty=qty,
                    price=price,
                    last_qty=fill_qty,
                    last_px=price,
                    leaves_qty=result.get("remainingQty", 0),
                    cum_qty=fill_qty,
                )
        else:
            self._send_reject(cl_ord_id, symbol, side_fix, qty, "Engine unavailable")

    def _handle_cancel(self, fields):
        orig_cl_ord_id = fields.get(41, "")
        tracked = self.tracked_orders.get(orig_cl_ord_id, {})
        order_id = tracked.get("orderId")

        if not order_id:
            self._send_cancel_reject(orig_cl_ord_id, "Unknown order")
            return

        result = self._post_order("/order/cancel", {"orderId": order_id})
        if result and not result.get("error"):
            self._send_exec_report(
                cl_ord_id=orig_cl_ord_id,
                order_id=str(order_id),
                exec_type="4",
                ord_status="4",
                symbol=tracked.get("symbol", ""),
                side="1" if tracked.get("side") == "Buy" else "2",
                qty=tracked.get("quantity", 0),
                price=tracked.get("price", 0),
                last_qty=0, last_px=0,
                leaves_qty=0,
                cum_qty=tracked.get("quantity", 0) - tracked.get("remainingQty", 0),
            )
        else:
            self._send_cancel_reject(orig_cl_ord_id, "Cancel failed")

    def _handle_amend(self, fields):
        orig_cl_ord_id = fields.get(41, "")
        tracked = self.tracked_orders.get(orig_cl_ord_id, {})
        order_id = tracked.get("orderId")

        if not order_id:
            self._send_cancel_reject(orig_cl_ord_id, "Unknown order")
            return

        amend_data = {"orderId": order_id}
        if 44 in fields:
            amend_data["price"] = float(fields[44])
        if 38 in fields:
            amend_data["quantity"] = int(fields[38])

        result = self._post_order("/order/amend", amend_data)
        if result and not result.get("error"):
            self.tracked_orders[orig_cl_ord_id] = result
            self._send_exec_report(
                cl_ord_id=orig_cl_ord_id,
                order_id=str(order_id),
                exec_type="5",
                ord_status=self._map_status(result.get("status", "New")),
                symbol=tracked.get("symbol", ""),
                side="1" if tracked.get("side") == "Buy" else "2",
                qty=result.get("quantity", 0),
                price=result.get("price", 0),
                last_qty=0, last_px=0,
                leaves_qty=result.get("remainingQty", 0),
                cum_qty=result.get("quantity", 0) - result.get("remainingQty", 0),
            )

    def _send_exec_report(self, cl_ord_id, order_id, exec_type, ord_status,
                          symbol, side, qty, price, last_qty, last_px,
                          leaves_qty, cum_qty):
        self.send("8", {
            37: order_id,
            11: cl_ord_id,
            17: f"EX-{int(time.time()*1000)}",
            150: exec_type,
            39: ord_status,
            55: symbol,
            54: side,
            38: qty,
            44: price,
            32: last_qty,
            31: last_px,
            151: leaves_qty,
            14: cum_qty,
            6: price,
        })

    def _send_reject(self, cl_ord_id, symbol, side, qty, reason):
        self.send("8", {
            37: "NONE",
            11: cl_ord_id,
            17: f"REJ-{int(time.time()*1000)}",
            150: "8",
            39: "8",
            55: symbol,
            54: side,
            38: qty,
            58: reason,
            151: qty,
            14: 0,
            6: 0,
        })

    def _send_cancel_reject(self, cl_ord_id, reason):
        self.send("9", {
            11: cl_ord_id,
            41: cl_ord_id,
            39: "8",
            434: "1",
            58: reason,
        })

    def _resolve_symbol(self, name):
        from shared.config import SYMBOLS
        for sid, info in SYMBOLS.items():
            if info["name"] == name:
                return sid
        return None

    def _map_status(self, status):
        return {"New": "0", "PartiallyFilled": "1", "Filled": "2",
                "Cancelled": "4", "Rejected": "8"}.get(status, "0")

    def _post_order(self, path, data):
        try:
            body = json.dumps(data).encode()
            req = urllib.request.Request(
                f"{DASHBOARD_URL}{path}",
                data=body,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=5) as resp:
                return json.loads(resp.read())
        except Exception as e:
            print(f"[FIX] Dashboard request failed: {e}")
            return None


class FIXServer:
    def __init__(self, host="0.0.0.0", port=9001):
        self.host = host
        self.port = port
        self.sessions = []
        self.lock = threading.Lock()
        self.running = False

    def start(self):
        self.running = True
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.host, self.port))
        self.sock.listen(5)
        self.sock.settimeout(1.0)

        print(f"[FIX] Server listening on {self.host}:{self.port}")
        print(f"[FIX] Dashboard URL: {DASHBOARD_URL}")

        while self.running:
            try:
                conn, addr = self.sock.accept()
                conn.settimeout(30.0)
                session = FIXSession(conn, addr, self)
                with self.lock:
                    self.sessions.append(session)
                t = threading.Thread(target=session.handle, daemon=True)
                t.start()
                print(f"[FIX] New connection from {addr}")
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(f"[FIX] Accept error: {e}")

    def remove_session(self, session):
        with self.lock:
            if session in self.sessions:
                self.sessions.remove(session)

    def stop(self):
        self.running = False
        with self.lock:
            for s in self.sessions:
                s.running = False
        self.sock.close()


# ── Simple FIX test client ──────────────────────────────────────────

def fix_test_client(host="localhost", port=9001):
    """Send a test order via FIX protocol."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))

    def send_fix(fields):
        fields[8] = FIX_VERSION
        raw = build_fix(fields)
        sock.sendall(raw.encode("ascii"))

    def recv_fix():
        data = sock.recv(4096)
        return parse_fix(data.decode("ascii", errors="replace"))

    send_fix({35: "A", 49: "TESTCLIENT", 56: "EUNEX", 34: 1,
              52: time.strftime("%Y%m%d-%H:%M:%S"), 98: 0, 108: 30})
    print("Logon response:", recv_fix())

    cl_ord_id = f"TEST-{int(time.time()*1000)}"
    send_fix({35: "D", 49: "TESTCLIENT", 56: "EUNEX", 34: 2,
              52: time.strftime("%Y%m%d-%H:%M:%S"),
              11: cl_ord_id, 55: "AAPL", 54: "1", 40: "2",
              38: 100, 44: 185.50, 59: "0"})
    print("Exec report:", recv_fix())

    send_fix({35: "5", 49: "TESTCLIENT", 56: "EUNEX", 34: 3,
              52: time.strftime("%Y%m%d-%H:%M:%S")})
    print("Logout response:", recv_fix())
    sock.close()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "test":
        host = sys.argv[2] if len(sys.argv) > 2 else "localhost"
        fix_test_client(host, FIX_PORT)
    else:
        server = FIXServer(port=FIX_PORT)
        try:
            server.start()
        except KeyboardInterrupt:
            server.stop()
            print("\n[FIX] Server stopped")
