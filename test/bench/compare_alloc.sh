#!/usr/bin/env bash
# 对比 system malloc 与 jemalloc 后端的性能。
# 两份配置仅 allocator 一行不同，持久化均关闭以隔离分配器影响。
set -euo pipefail

ROOT="/home/nix/code/tinykv"
SERVER="$ROOT/build/tinykv_server"
BENCH="$ROOT/test/bench/bench.py"
PORT=6390
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null || true' EXIT

# 基准参数（与 README 一致）
export HOST=127.0.0.1 PORT CONNS=50 PIPELINE=16 DURATION=5 VALUE_LEN=16 READ_RATIO=0.5

make_conf() {  # $1=allocator
  cat > "$TMP/kvs.conf" <<EOF
bind 0.0.0.0
port $PORT
network reactor
allocator $1
appendonly no
snapshot_enabled no
EOF
}

run_case() {  # $1=label  $2=OPS  $3=KEYSPACE
  OPS="$2" KEYSPACE="$3" python3 "$BENCH" \
    | awk -v l="$1" '/^QPS:/{q=$2} /^Latency/{a=$0} END{
        sub(/.*avg=/,"",a); sub(/ .*/,"",a);
        printf "  %-22s QPS=%-10.0f avg=%sms\n", l, q, a}'
}

for backend in system jemalloc; do
  make_conf "$backend"
  "$SERVER" "$TMP/kvs.conf" >/dev/null 2>&1 &
  SRV_PID=$!
  sleep 0.6
  echo "== allocator=$backend =="
  run_case "Hash(HSET/HGET)"      "HSET,HGET" 100000
  run_case "RBTree(RSET/RGET)"    "RSET,RGET" 100000
  run_case "Array(SET/GET) k=1k"  "SET,GET"   1000
  # 高分配压力场景：写为主 + 大 value，逼出分配器开销
  ( export VALUE_LEN=256 READ_RATIO=0.0
    run_case "Hash write v=256B"  "HSET,HGET" 100000 )
  kill "$SRV_PID" 2>/dev/null || true
  wait "$SRV_PID" 2>/dev/null || true
  SRV_PID=""
  echo
done
