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


def default_payload(index):
    prefix = f"vfio-echo-loop-{index:04d}-".encode()
    return prefix + bytes(range(96))


def main():
    parser = argparse.ArgumentParser(
        description="Send several raw Ethernet frames and print echo replies."
    )
    parser.add_argument("--iface", default=DEFAULT_IFACE)
    parser.add_argument("--src-mac", default=DEFAULT_SRC_MAC)
    parser.add_argument("--dst-mac", default=DEFAULT_DST_MAC)
    parser.add_argument("--ethertype", default=hex(DEFAULT_ETHERTYPE))
    parser.add_argument("--payload-hex")
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--expect", type=int)
    parser.add_argument("--interval", type=float, default=0.1)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--dump-bytes", type=int, default=128)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if args.count <= 0:
        raise ValueError("count must be greater than 0")
    expect = args.count if args.expect is None else args.expect
    if expect <= 0:
        raise ValueError("expect must be greater than 0")

    src_mac = parse_mac(args.src_mac)
    dst_mac = parse_mac(args.dst_mac)
    ethertype = int(args.ethertype, 0)
    if not 0 <= ethertype <= 0xFFFF:
        raise ValueError("ethertype must fit in 16 bits")

    sock = open_raw_socket(args.iface)

    for index in range(args.count):
        payload = (
            parse_hex_bytes(args.payload_hex)
            if args.payload_hex
            else default_payload(index)
        )
        frame = build_frame(dst_mac, src_mac, ethertype, payload)
        if index == 0:
            describe_frame("First TX frame", frame, args.dump_bytes)
        sent = sock.send(frame)
        print(f"sent {sent} bytes ({index + 1}/{args.count})")
        if index + 1 < args.count:
            time.sleep(args.interval)

    print(
        "waiting for echoes "
        f"dst={format_mac(src_mac)} src={format_mac(dst_mac)} "
        f"ethertype=0x{ethertype:04x} expect={expect} timeout={args.timeout}s"
    )
    received = collect_echoes(
        sock,
        src_mac,
        dst_mac,
        ethertype,
        expect,
        args.timeout,
        args.dump_bytes,
        args.verbose,
    )
    print(f"received {received}/{expect} echo frames")
    return 0 if received == expect else 1


if __name__ == "__main__":
    raise SystemExit(main())
