#!/usr/bin/env python3
"""tinykv 压力测试客户端。

通过 RESP 协议向 tinykv_server 发起并发 pipeline 压测，统计 QPS 与延迟分位。

用法示例：
    # 先启动服务
    ./build/tinykv_server kvs.conf

    # 默认命名空间（数组），读写各半
    python3 test/bench/bench.py

    # Hash 命名空间，50 连接、跑 5 秒
    OPS=HSET,HGET CONNS=50 DURATION=5 python3 test/bench/bench.py

    # RBTree 命名空间
    OPS=RSET,RGET python3 test/bench/bench.py

可调环境变量见下方常量。
"""
import asyncio
import os
import random
import statistics
import time
from dataclasses import dataclass, field

HOST = os.getenv("HOST", "127.0.0.1")
PORT = int(os.getenv("PORT", "6380"))

# 例：OPS=HSET,HGET 或 OPS=RSET,RGET 或 OPS=SET,GET
OPS = [x.strip().upper() for x in os.getenv("OPS", "SET,GET").split(",") if x.strip()]
CONNS = int(os.getenv("CONNS", "100"))          # 并发连接数（连接=客户端）
DURATION = float(os.getenv("DURATION", "10"))   # 压测时长（秒）
PIPELINE = int(os.getenv("PIPELINE", "16"))     # pipeline 深度（每批请求条数）
KEYSPACE = int(os.getenv("KEYSPACE", "100000")) # key 空间大小
VALUE_LEN = int(os.getenv("VALUE_LEN", "16"))   # value 长度
READ_RATIO = float(os.getenv("READ_RATIO", "0.5"))  # GET 类操作比例（0~1）


def resp_array(parts):
    """把字符串列表编码成 RESP Array of Bulk Strings。"""
    out = [f"*{len(parts)}\r\n".encode()]
    for p in parts:
        b = p.encode()
        out.append(f"${len(b)}\r\n".encode())
        out.append(b)
        out.append(b"\r\n")
    return b"".join(out)


def rand_value(n=VALUE_LEN):
    alphabet = "abcdefghijklmnopqrstuvwxyz0123456789"
    return "".join(random.choice(alphabet) for _ in range(n))


async def read_one_reply(reader: asyncio.StreamReader):
    """最小 RESP 回复读取：+simple / -error / :int / $len\\r\\ndata\\r\\n（len 可为 -1）。"""
    first = await reader.readexactly(1)
    if first in (b"+", b"-", b":"):
        await reader.readuntil(b"\r\n")
        return
    if first == b"$":
        line = await reader.readuntil(b"\r\n")
        n = int(line[:-2])
        if n == -1:
            return
        await reader.readexactly(n + 2)  # data + \r\n
        return
    if first == b"*":
        # 当前 server 不会返回 array，简单跳过 header 行
        await reader.readuntil(b"\r\n")
        return
    await reader.readuntil(b"\r\n")


def pick_op():
    """根据 READ_RATIO 在 SET/GET（或 HSET/HGET、RSET/RGET）之间选择操作。"""
    if len(OPS) >= 3:
        return random.choice(OPS)

    if len(OPS) == 2:
        a, b = OPS[0], OPS[1]
        # 让含 GET 的那个按 READ_RATIO 出现
        if "GET" in a and "GET" not in b:
            return a if random.random() < READ_RATIO else b
        if "GET" in b and "GET" not in a:
            return b if random.random() < READ_RATIO else a
        return random.choice([a, b])

    return OPS[0] if OPS else "PING"


@dataclass
class Stats:
    ops: int = 0
    lat_ms: list = field(default_factory=list)


async def worker(deadline: float, stats: Stats):
    reader, writer = await asyncio.open_connection(HOST, PORT)
    try:
        # 预热 ping
        writer.write(resp_array(["PING"]))
        await writer.drain()
        await read_one_reply(reader)

        while time.perf_counter() < deadline:
            batch = []
            for _ in range(PIPELINE):
                op = pick_op()
                k = f"k{random.randrange(KEYSPACE)}"
                if op.endswith("SET"):
                    batch.append(resp_array([op, k, rand_value()]))
                elif op.endswith("GET") or op.endswith("DEL") or op.endswith("EXISTS"):
                    batch.append(resp_array([op, k]))
                else:
                    batch.append(resp_array(["PING"]))

            payload = b"".join(batch)

            t0 = time.perf_counter()
            writer.write(payload)
            await writer.drain()
            for _ in range(PIPELINE):
                await read_one_reply(reader)
            t1 = time.perf_counter()

            stats.ops += PIPELINE
            stats.lat_ms.append((t1 - t0) * 1000.0 / PIPELINE)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


def pct(sorted_lats, p):
    if not sorted_lats:
        return float("nan")
    i = int((p / 100.0) * (len(sorted_lats) - 1))
    return sorted_lats[i]


async def main():
    random.seed(42)

    deadline = time.perf_counter() + DURATION
    all_stats = [Stats() for _ in range(CONNS)]
    tasks = [asyncio.create_task(worker(deadline, all_stats[i])) for i in range(CONNS)]
    await asyncio.gather(*tasks)

    total_ops = sum(s.ops for s in all_stats)
    qps = total_ops / DURATION
    lats = sorted(x for s in all_stats for x in s.lat_ms)

    print(f"HOST={HOST} PORT={PORT}")
    print(f"OPS={OPS} READ_RATIO={READ_RATIO}")
    print(f"CONNS={CONNS} PIPELINE={PIPELINE} DURATION={DURATION}s "
          f"KEYSPACE={KEYSPACE} VALUE_LEN={VALUE_LEN}")
    print(f"Total ops: {total_ops}")
    print(f"QPS: {qps:.2f}")
    if lats:
        print(
            "Latency(ms): "
            f"avg={statistics.mean(lats):.4f} "
            f"p50={pct(lats, 50):.4f} "
            f"p95={pct(lats, 95):.4f} "
            f"p99={pct(lats, 99):.4f} "
            f"max={lats[-1]:.4f}"
        )


if __name__ == "__main__":
    asyncio.run(main())
