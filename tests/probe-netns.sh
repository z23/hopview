#!/usr/bin/env bash
# Exercise Hopline across a virtual router without sending packets to the host network.
# Requires Linux user/network namespaces, iproute2, util-linux, iputils and Python 3.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
binary=$(realpath "${1:-$root/build/hopline}")
[[ -x "$binary" ]] || { echo "Build Hopline before running this test." >&2; exit 1; }

unshare --user --map-root-user --net bash -seu -o pipefail -- "$binary" <<'LAB'
binary=$1
unshare --net sleep 120 &
router=$!
unshare --net sleep 120 &
dest=$!
trap 'kill "$router" "$dest" 2>/dev/null || true; wait || true' EXIT
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

# New namespaces normally deny ping sockets. UDP works with this default.
# Warm up neighbour discovery using a UDP datagram, which requires no ping socket.
python3 -c 'import socket; socket.socket(socket.AF_INET6,socket.SOCK_DGRAM).sendto(b"test",("2001:db8:1::20",33333))'
sleep 2
check() {
    local family=$1 address=$2
    shift 2
    "$binary" --report "$address" --family "$family" --interval 250 --seconds 3 |
        python3 -c '
import json, sys
report = json.load(sys.stdin)
expected = sys.argv[1:]
rows = report["rows"]
assert len(rows) == len(expected), rows
for i, (row, address) in enumerate(zip(rows, expected), start=1):
    assert (row["hop"], row["address"], row["destination"]) == (i, address, i == len(expected)), row
    assert 0 <= row["loss"] < 100 and 0 <= row["avg"], row
print(report["status"], " -> ".join(expected))
' "$@"
}
check 4 192.0.2.20 192.0.2.2 192.0.2.20
check 6 2001:db8:1::20 2001:db8::2 2001:db8:1::20
sysctl -qw 'net.ipv4.ping_group_range=0 0'
check 4 192.0.2.20 192.0.2.2 192.0.2.20
check 6 2001:db8:1::20 2001:db8::2 2001:db8:1::20
check 6 fe80::20%hl-left fe80::20%hl-left

# A one-second capture still records a timeout when its reply deadline is later.
d sysctl -qw net.ipv4.icmp_echo_ignore_all=1
"$binary" --report 192.0.2.20 --family 4 --interval 1000 --seconds 1 |
    python3 -c '
import json, sys
rows = json.load(sys.stdin)["rows"]
assert rows[0]["address"] == "192.0.2.2", rows
assert rows[1]["sent"] >= 1 and rows[1]["loss"] == 100 and rows[1]["avg"] == -1, rows
print("Short capture recorded unanswered probes.")
'
LAB
