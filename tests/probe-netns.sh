#!/usr/bin/env bash
# Exercise Hopview across a virtual router without sending packets to the host network.
# The test needs unprivileged user and network namespaces to create its isolated links.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
binary=$(realpath "${1:-$root/build/hopview}")
[[ -x "$binary" ]] || { echo "Build Hopview before running this test." >&2; exit 1; }

unshare --user --map-root-user --net bash -seu -o pipefail -- "$binary" <<'LAB'
binary=$1
unshare --net sleep 180 &
router=$!
unshare --net sleep 180 &
dest=$!
labdir=$(mktemp -d)
server=""
trap 'kill "$router" "$dest" ${server:+"$server"} 2>/dev/null || true; wait || true; rm -rf -- "$labdir"' EXIT
# The child must have entered its new namespace before the peers are installed.
for pid in "$router" "$dest"; do
    for _ in {1..100}; do
        [[ $(readlink "/proc/$pid/ns/net") != $(readlink /proc/self/ns/net) ]] && break
        sleep 0.02
    done
    [[ $(readlink "/proc/$pid/ns/net") != $(readlink /proc/self/ns/net) ]]
done
r() { nsenter -t "$router" -n "$@"; }
d() { nsenter -t "$dest" -n "$@"; }
ip link set lo up
r ip link set lo up
d ip link set lo up
ip link add hl-left type veth peer name r-left netns "$router"
r ip link add r-right type veth peer name d-left netns "$dest"
ip link set hl-left up
r ip link set r-left up
r ip link set r-right up
d ip link set d-left up
ip addr add 192.0.2.1/30 dev hl-left
r ip addr add 192.0.2.2/30 dev r-left
r ip addr add 192.0.2.17/28 dev r-right
d ip addr add 192.0.2.20/28 dev d-left
ip -6 addr add 2001:db8::1/64 dev hl-left nodad
r ip -6 addr add 2001:db8::2/64 dev r-left nodad
r ip -6 addr add fe80::20/64 dev r-left nodad
r ip -6 addr add 2001:db8:1::2/64 dev r-right nodad
d ip -6 addr add 2001:db8:1::20/64 dev d-left nodad
ip route add 192.0.2.16/28 via 192.0.2.2
d ip route add 192.0.2.0/30 via 192.0.2.17
ip -6 route add 2001:db8:1::/64 via 2001:db8::2
d ip -6 route add 2001:db8::/64 via 2001:db8:1::2
r sysctl -qw net.ipv4.ip_forward=1
r sysctl -qw net.ipv6.conf.all.forwarding=1

# Only topology setup needs namespace capabilities. Exercise the real CLI with none.
trace() { setpriv --bounding-set=-all --no-new-privs "$binary" "$@"; }
# New namespaces normally deny ICMP echo sockets, so use a UDP datagram to warm up neighbour discovery.
python3 -c 'import socket; socket.socket(socket.AF_INET6,socket.SOCK_DGRAM).sendto(b"test",("2001:db8:1::20",33333))'
sleep 2
method=auto
port=0
check() {
    local family=$1 address=$2
    shift 2
    local -a args=(--protocol "$method" --interface hl-left)
    if ((port)); then args+=(--port "$port"); fi
    trace --report "$address" --family "$family" --interval 250 --seconds 2 "${args[@]}" |
        python3 -c '
import json, sys
report = json.load(sys.stdin)
method, port = sys.argv[1], int(sys.argv[2])
expected = sys.argv[3:]
assert report["protocol"] == method and report["interfaceName"] == "hl-left", report
assert report["effectiveProtocol"] == ("udp" if method == "auto" else method), report
assert report["resolvedAddress"] == expected[-1], report
assert report["port"] == (port or {"auto": 33434, "icmp": 0, "tcp": 443, "udp": 33434}[method]), report
rows = report["rows"]
assert len(rows) == len(expected), rows
for i, (row, address) in enumerate(zip(rows, expected), start=1):
    assert (row["hop"], row["address"], row["destination"]) == (i, address, i == len(expected)), row
    assert 0 <= row["loss"] < 100 and 0 <= row["avg"], row
print(report["status"], " -> ".join(expected))
' "$method" "$port" "$@"
}
check 4 192.0.2.20 192.0.2.2 192.0.2.20
check 6 2001:db8:1::20 2001:db8::2 2001:db8:1::20
# Explicit UDP still works when ICMP is available. Test closed and listening TCP ports.
method=tcp
port=41234
check 4 192.0.2.20 192.0.2.2 192.0.2.20
check 6 2001:db8:1::20 2001:db8::2 2001:db8:1::20
nsenter -t "$dest" -n python3 -u - "$labdir" <<'SERVER' &
import pathlib, selectors, socket, sys
lab = pathlib.Path(sys.argv[1])
sel = selectors.DefaultSelector()
for kind, port in ((socket.SOCK_STREAM, 41235), (socket.SOCK_DGRAM, 41235), (socket.SOCK_DGRAM, 41236)):
    sock = socket.socket(socket.AF_INET6, kind)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    sock.bind(("::", port))
    if kind == socket.SOCK_STREAM: sock.listen()
    sel.register(sock, selectors.EVENT_READ, (kind, port))
(lab / "ready").touch()
while True:
    for key, _ in sel.select():
        kind, port = key.data
        if kind == socket.SOCK_STREAM:
            client, peer = key.fileobj.accept()
            client.close()
            proto = "tcp"
        else:
            data, peer = key.fileobj.recvfrom(2048)
            # This deliberately exercises application responses without an echoed Hopview tag.
            key.fileobj.sendto(data if port == 41236 else b"reply", peer)
            proto = "udp"
        family = "4" if peer[0].startswith("::ffff:") else "6"
        (lab / (proto + family)).touch()
        if port == 41236:
            with (lab / ("ports" + family)).open("a") as f:
                print(peer[1], file=f)
SERVER
server=$!
for _ in {1..100}; do [[ -e "$labdir/ready" ]] && break; sleep 0.02; done
[[ -e "$labdir/ready" ]]
sysctl -qw 'net.ipv4.ping_group_range=0 0'
port=41235
for method in tcp udp; do
    check 4 192.0.2.20 192.0.2.2 192.0.2.20
    check 6 2001:db8:1::20 2001:db8::2 2001:db8:1::20
    [[ -e "$labdir/${method}4" && -e "$labdir/${method}6" ]]
done
method=udp
port=41236
check 4 192.0.2.20 192.0.2.2 192.0.2.20
check 6 2001:db8:1::20 2001:db8::2 2001:db8:1::20
for family in 4 6; do
    [[ $(sort -u "$labdir/ports$family" | wc -l) -eq 1 ]]
done
port=0
method=icmp
check 4 192.0.2.20 192.0.2.2 192.0.2.20
check 6 2001:db8:1::20 2001:db8::2 2001:db8:1::20
check 6 fe80::20 fe80::20%hl-left
if trace --report 192.0.2.20 --interface hl-missing --seconds 1 >"$labdir/invalid"; then
    echo "Unknown source interface was accepted" >&2
    exit 1
fi
python3 -c 'import json,sys; assert "Unknown network interface" in json.load(open(sys.argv[1]))["status"]' "$labdir/invalid"

# A one-second capture still records a timeout when its reply deadline is later.
d sysctl -qw net.ipv4.icmp_echo_ignore_all=1
trace --report 192.0.2.20 --family 4 --interval 1000 --seconds 1 |
    python3 -c '
import json, sys
rows = json.load(sys.stdin)["rows"]
assert rows[0]["address"] == "192.0.2.2", rows
assert rows[1]["sent"] >= 1 and rows[1]["loss"] == 100 and rows[1]["avg"] == -1, rows
print("Short capture recorded unanswered probes.")
'
LAB
