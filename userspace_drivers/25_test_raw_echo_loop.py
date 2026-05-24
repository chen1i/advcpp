#!/usr/bin/env python3
import argparse
import select
import socket
import time


ETH_P_ALL = 0x0003
DEFAULT_IFACE = "ens6f1v0"
DEFAULT_SRC_MAC = "fe:bf:30:01:30:01"
DEFAULT_DST_MAC = "fe:bf:30:01:30:04"
DEFAULT_ETHERTYPE = 0x88B5


def parse_mac(text):
    hex_text = text.replace(":", "").replace("-", "").lower()
    if len(hex_text) != 12:
        raise ValueError(f"invalid MAC address: {text}")
    try:
        return bytes.fromhex(hex_text)
    except ValueError as exc:
        raise ValueError(f"invalid MAC address: {text}") from exc


def format_mac(mac):
    return ":".join(f"{byte:02x}" for byte in mac)


def parse_hex_bytes(text):
    hex_text = "".join(ch for ch in text if ch not in " :-\n\t")
    if len(hex_text) % 2 != 0:
        raise ValueError("hex payload has an odd number of digits")
    return bytes.fromhex(hex_text)


def default_payload(index):
    prefix = f"vfio-echo-loop-{index:04d}-".encode()
    return prefix + bytes(range(96))


def hexdump(data, max_bytes):
    shown = data[:max_bytes]
    for offset in range(0, len(shown), 16):
        line = shown[offset : offset + 16]
        hex_part = " ".join(f"{byte:02x}" for byte in line)
        hex_part = f"{hex_part:<47}"
        ascii_part = "".join(chr(byte) if 32 <= byte < 127 else "." for byte in line)
        print(f"  {offset:04x}: {hex_part}  {ascii_part}")
    if len(data) > len(shown):
        print(f"  ... {len(data) - len(shown)} more bytes not shown")


def describe_frame(label, frame, dump_bytes):
    print(f"{label}: length {len(frame)}")
    if len(frame) < 14:
        print("  Ethernet: <too short>")
        hexdump(frame, dump_bytes)
        return

    dst = frame[0:6]
    src = frame[6:12]
    ethertype = int.from_bytes(frame[12:14], "big")
    print(f"  dst: {format_mac(dst)}")
    print(f"  src: {format_mac(src)}")
    print(f"  ethertype: 0x{ethertype:04x}")
    print(f"  payload bytes: {len(frame) - 14}")
    hexdump(frame, dump_bytes)


def packet_type_name(packet_type):
    names = {
        getattr(socket, "PACKET_HOST", 0): "HOST",
        getattr(socket, "PACKET_BROADCAST", 1): "BROADCAST",
        getattr(socket, "PACKET_MULTICAST", 2): "MULTICAST",
        getattr(socket, "PACKET_OTHERHOST", 3): "OTHERHOST",
        getattr(socket, "PACKET_OUTGOING", 4): "OUTGOING",
    }
    return names.get(packet_type, str(packet_type))


def build_frame(dst_mac, src_mac, ethertype, payload):
    frame = dst_mac + src_mac + ethertype.to_bytes(2, "big") + payload
    if len(frame) < 60:
        frame += bytes(60 - len(frame))
    return frame


def collect_echoes(sock, src_mac, dst_mac, ethertype, expect, timeout, dump_bytes, verbose):
    deadline = time.monotonic() + timeout
    received = 0

    while received < expect:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break

        readable, _, _ = select.select([sock], [], [], remaining)
        if not readable:
            break

        frame, addr = sock.recvfrom(65535)
        packet_type = addr[2] if len(addr) > 2 else None
        if packet_type == getattr(socket, "PACKET_OUTGOING", 4):
            if verbose:
                print(f"skip outgoing frame length {len(frame)}")
            continue
        if len(frame) < 14:
            if verbose:
                print(f"skip short frame length {len(frame)}")
            continue

        rx_dst = frame[0:6]
        rx_src = frame[6:12]
        rx_ethertype = int.from_bytes(frame[12:14], "big")
        is_echo = rx_dst == src_mac and rx_src == dst_mac and rx_ethertype == ethertype
        if not is_echo:
            if verbose:
                ptype = packet_type_name(packet_type)
                print(
                    "skip frame "
                    f"pkttype={ptype} dst={format_mac(rx_dst)} "
                    f"src={format_mac(rx_src)} ethertype=0x{rx_ethertype:04x}"
                )
            continue

        received += 1
        ptype = packet_type_name(packet_type)
        print(f"received echo {received}/{expect} on {addr[0]} pkttype={ptype}")
        describe_frame("RX echo frame", frame, dump_bytes)

    return received


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

    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sock.bind((args.iface, 0))

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
