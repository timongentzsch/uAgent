#!/bin/sh
# The network the agent's container joins: public addresses stay reachable,
# this machine, its LAN and its tailnet do not, unless UAGENT_NETWORK_ALLOW
# names them ("host", "host:port" or a CIDR, separated by spaces or commas).
# UAGENT_NETWORK=open loads no rules.
set -eu

if [ "${UAGENT_NETWORK:-private}" = open ]; then
  echo "network: open; the agent reaches whatever this Docker host reaches"
  exec sleep infinity
fi

allow=
for entry in $(printf '%s' "${UAGENT_NETWORK_ALLOW:-}" | tr ',' ' '); do
  port=
  case $entry in
    */*) addresses=$entry ;;
    *:*) port=${entry##*:}; entry=${entry%:*}
         addresses=$(getent ahostsv4 "$entry" | awk '{print $1}' | sort -u) ;;
    *) addresses=$(getent ahostsv4 "$entry" | awk '{print $1}' | sort -u) ;;
  esac
  [ -n "$addresses" ] || { echo "network: cannot resolve $entry" >&2; exit 1; }
  for address in $addresses; do
    allow="$allow
    ip daddr $address ${port:+tcp dport $port }accept"
  done
done

# Docker's resolver answers on loopback and asks these from here.
for server in $(sed -n 's/^# ExtServers: //p' /etc/resolv.conf |
                grep -oE '[0-9]+(\.[0-9]+){3}'); do
  allow="$allow
    ip daddr $server meta l4proto { tcp, udp } th dport 53 accept"
done

# The bridge's own address, wherever Docker put the network.
gateway=
hex=$(awk '$2 == "00000000" { print $3; exit }' /proc/net/route)
if [ -n "$hex" ]; then
  gateway=$(printf '%d.%d.%d.%d' "0x$(echo "$hex" | cut -c7-8)" \
    "0x$(echo "$hex" | cut -c5-6)" "0x$(echo "$hex" | cut -c3-4)" \
    "0x$(echo "$hex" | cut -c1-2)")
fi

nft -f - <<RULES
table inet uagent
delete table inet uagent
table inet uagent {
  chain output {
    type filter hook output priority filter; policy accept;
    oifname "lo" accept
    ct state established,related accept$allow
    ${gateway:+ip daddr $gateway reject with icmp type admin-prohibited}
    ip daddr { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 169.254.0.0/16, 172.16.0.0/12, 192.168.0.0/16, 224.0.0.0/3 } reject with icmp type admin-prohibited
    ip6 daddr { fc00::/7, fe80::/10, ff00::/8 } reject with icmpv6 type admin-prohibited
  }
}
RULES
echo "network: private addresses refused${UAGENT_NETWORK_ALLOW:+; allowed: $UAGENT_NETWORK_ALLOW}"
exec sleep infinity
