import socket, time

iface = "ens6f1v0"
dst = bytes.fromhex("febf30013004")
src = bytes.fromhex("febf30013001")
ethertype = bytes.fromhex("88b5")
payload = b"vfio-rx-test-" + bytes(range(100))
frame = dst + src + ethertype + payload

print("frame length", len(frame))
assert len(frame) >= 60

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind((iface, 0))
for _ in range(3):
    print("sent", s.send(frame))
    time.sleep(0.1)
