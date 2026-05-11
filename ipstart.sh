sudo sysctl -w net.ipv4.ip_forward=1

HOST_IF=$(ip route | awk '/default/ {print $5; exit}')

sudo iptables -t nat -A POSTROUTING -s 192.168.50.0/24 -o "$HOST_IF" -j MASQUERADE
sudo iptables -A FORWARD -i br0 -o "$HOST_IF" -j ACCEPT
sudo iptables -A FORWARD -i "$HOST_IF" -o br0 -m state --state RELATED,ESTABLISHED -j ACCEPT
