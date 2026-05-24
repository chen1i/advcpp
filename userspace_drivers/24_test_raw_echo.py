#!/usr/bin/env python3
import argparse
import time

from raw_eth import (
    build_frame,
    collect_echoes,
    describe_frame,
    format_mac,
    open_raw_socket,
    parse_hex_bytes,
    parse_mac,
)


DEFAULT_IFACE = "ens6f1v0"
DEFAULT_SRC_MAC = "fe:bf:30:01:30:01"
DEFAULT_DST_MAC = "fe:bf:30:01:30:04"
DEFAULT_ETHERTYPE = 0x88B5


def default_payload():
    return b"vfio-rx-test-" + bytes(range(100))


def main():
    parser = argparse.ArgumentParser(
        description="Send a raw Ethernet test frame and print the echo reply."
    )
    parser.add_argument("--iface", default=DEFAULT_IFACE)
    parser.add_argument("--src-mac", default=DEFAULT_SRC_MAC)
    parser.add_argument("--dst-mac", default=DEFAULT_DST_MAC)
    parser.add_argument("--ethertype", default=hex(DEFAULT_ETHERTYPE))
    parser.add_argument("--payload-hex")
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--interval", type=float, default=0.1)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--dump-bytes", type=int, default=192)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    src_mac = parse_mac(args.src_mac)
    dst_mac = parse_mac(args.dst_mac)
    ethertype = int(args.ethertype, 0)
    if not 0 <= ethertype <= 0xFFFF:
        raise ValueError("ethertype must fit in 16 bits")

    payload = parse_hex_bytes(args.payload_hex) if args.payload_hex else default_payload()
    frame = build_frame(dst_mac, src_mac, ethertype, payload)
    sock = open_raw_socket(args.iface)

    describe_frame("TX frame", frame, args.dump_bytes)
    for index in range(args.count):
        sent = sock.send(frame)
        print(f"sent {sent} bytes ({index + 1}/{args.count})")
        if index + 1 < args.count:
            time.sleep(args.interval)

    print(
        "waiting for echo "
        f"dst={format_mac(src_mac)} src={format_mac(dst_mac)} "
        f"ethertype=0x{ethertype:04x} timeout={args.timeout}s"
    )
    received = collect_echoes(
        sock,
        src_mac,
        dst_mac,
        ethertype,
        1,
        args.timeout,
        args.dump_bytes,
        args.verbose,
    )
    if received == 1:
        return 0

    print("timed out waiting for echo")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
