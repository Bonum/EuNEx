#!/bin/bash
# ════════════════════════════════════════════════════════════════════
# EuNEx — Start all services (Dashboard, FIX Gateway, Clearing House)
#
# Usage:
#   ./run.sh              # Start all services
#   ./run.sh dashboard    # Start dashboard only
#   ./run.sh fix          # Start FIX gateway only
#   ./run.sh ch           # Start clearing house only
#   ./run.sh stop         # Stop all services
# ════════════════════════════════════════════════════════════════════

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="${EUNEX_DATA_DIR:-$SCRIPT_DIR/data}"
LOG_DIR="$SCRIPT_DIR/logs"
PID_DIR="$SCRIPT_DIR/.pids"

DASHBOARD_PORT="${EUNEX_DASHBOARD_PORT:-8090}"
FIX_PORT="${EUNEX_FIX_PORT:-9001}"
CH_PORT="${EUNEX_CH_PORT:-8091}"

mkdir -p "$DATA_DIR" "$LOG_DIR" "$PID_DIR"

export PYTHONPATH="$SCRIPT_DIR:$PYTHONPATH"
export EUNEX_DATA_DIR="$DATA_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${CYAN}[EuNEx]${NC} $1"; }
ok()  { echo -e "${GREEN}[EuNEx]${NC} $1"; }
warn(){ echo -e "${YELLOW}[EuNEx]${NC} $1"; }
err() { echo -e "${RED}[EuNEx]${NC} $1"; }

check_python() {
    if command -v python3 &>/dev/null; then
        PYTHON=python3
    elif command -v python &>/dev/null; then
        PYTHON=python
    else
        err "Python not found. Install python3 and flask:"
        err "  pip install flask"
        exit 1
    fi

    if ! $PYTHON -c "import flask" 2>/dev/null; then
        warn "Flask not installed. Installing..."
        $PYTHON -m pip install flask --quiet
    fi
}

start_service() {
    local name=$1
    local cmd=$2
    local logfile="$LOG_DIR/$name.log"
    local pidfile="$PID_DIR/$name.pid"

    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        warn "$name already running (PID $(cat "$pidfile"))"
        return
    fi

    log "Starting $name..."
    $cmd > "$logfile" 2>&1 &
    local pid=$!
    echo $pid > "$pidfile"
    sleep 1

    if kill -0 $pid 2>/dev/null; then
        ok "$name started (PID $pid) — log: $logfile"
    else
        err "$name failed to start. Check $logfile"
    fi
}

stop_service() {
    local name=$1
    local pidfile="$PID_DIR/$name.pid"

    if [ -f "$pidfile" ]; then
        local pid=$(cat "$pidfile")
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            log "Stopped $name (PID $pid)"
        fi
        rm -f "$pidfile"
    fi
}

start_dashboard() {
    start_service "dashboard" "$PYTHON $SCRIPT_DIR/dashboard/app.py"
}

start_fix() {
    start_service "fix_gateway" "$PYTHON $SCRIPT_DIR/fix_gateway/fix_server.py"
}

start_ch() {
    start_service "clearing_house" "$PYTHON $SCRIPT_DIR/clearing_house/app.py"
}

start_all() {
    check_python

    echo ""
    echo -e "${CYAN}╔══════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║       EuNEx Trading Platform         ║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════╝${NC}"
    echo ""

    start_dashboard
    sleep 2
    start_fix
    start_ch

    echo ""
    ok "All services started!"
    echo ""
    echo -e "  Dashboard:       ${GREEN}http://localhost:${DASHBOARD_PORT}${NC}"
    echo -e "  FIX Gateway:     ${GREEN}localhost:${FIX_PORT}${NC} (TCP)"
    echo -e "  Clearing House:  ${GREEN}http://localhost:${CH_PORT}${NC}"
    echo ""
    echo -e "  Logs:  ${YELLOW}$LOG_DIR/${NC}"
    echo -e "  Data:  ${YELLOW}$DATA_DIR/${NC}"
    echo ""
    echo -e "  Stop:  ${CYAN}./run.sh stop${NC}"
    echo ""

    log "Press Ctrl+C to stop all services..."

    trap 'echo ""; stop_all; exit 0' INT TERM

    while true; do
        for svc in dashboard fix_gateway clearing_house; do
            pidfile="$PID_DIR/$svc.pid"
            if [ -f "$pidfile" ] && ! kill -0 "$(cat "$pidfile")" 2>/dev/null; then
                warn "$svc died — restarting..."
                rm -f "$pidfile"
                case $svc in
                    dashboard) start_dashboard ;;
                    fix_gateway) start_fix ;;
                    clearing_house) start_ch ;;
                esac
            fi
        done
        sleep 5
    done
}

stop_all() {
    log "Stopping all services..."
    stop_service "clearing_house"
    stop_service "fix_gateway"
    stop_service "dashboard"
    ok "All services stopped."
}

status_all() {
    for svc in dashboard fix_gateway clearing_house; do
        pidfile="$PID_DIR/$svc.pid"
        if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
            ok "$svc: running (PID $(cat "$pidfile"))"
        else
            warn "$svc: stopped"
        fi
    done
}

case "${1:-all}" in
    dashboard) check_python; start_dashboard; wait ;;
    fix)       check_python; start_fix; wait ;;
    ch)        check_python; start_ch; wait ;;
    stop)      stop_all ;;
    status)    status_all ;;
    all|"")    start_all ;;
    *)
        echo "Usage: $0 {all|dashboard|fix|ch|stop|status}"
        exit 1
        ;;
esac
