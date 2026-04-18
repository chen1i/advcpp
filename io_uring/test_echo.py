#!/usr/bin/env python3
"""Stress-test the echo server with concurrent clients."""

import argparse
import asyncio
import time


async def echo_client(id: int, host: str, port: int, rounds: int):
    reader, writer = await asyncio.open_connection(host, port)
    for r in range(rounds):
        msg = f"client {id} round {r}\n"
        writer.write(msg.encode())
        await writer.drain()
        data = await reader.readline()
        if data.decode() != msg:
            return id, False, f"mismatch: sent {msg!r}, got {data.decode()!r}"
    writer.close()
    await writer.wait_closed()
    return id, True, None


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-n", "--clients", type=int, default=100)
    parser.add_argument("-r", "--rounds", type=int, default=10,
                        help="messages per client")
    parser.add_argument("-p", "--port", type=int, default=9000)
    parser.add_argument("--host", default="localhost")
    args = parser.parse_args()

    print(f"Spawning {args.clients} clients, "
          f"{args.rounds} rounds each -> "
          f"{args.clients * args.rounds} total messages")

    t0 = time.monotonic()
    tasks = [echo_client(i, args.host, args.port, args.rounds)
             for i in range(args.clients)]
    results = await asyncio.gather(*tasks)
    elapsed = time.monotonic() - t0

    ok = sum(1 for _, success, _ in results if success)
    fail = sum(1 for _, success, _ in results if not success)

    for id, success, err in results:
        if not success:
            print(f"  FAIL client {id}: {err}")

    total = args.clients * args.rounds
    print(f"\n{ok}/{ok + fail} clients passed, "
          f"{total} messages in {elapsed:.3f}s "
          f"({total / elapsed:.0f} msg/s)")


if __name__ == "__main__":
    asyncio.run(main())
