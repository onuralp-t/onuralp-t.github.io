* [go back to README](README.md)
# Basics
* `sudo apt update -y & sudo apt upgrade -y`
* `sudo apt install base-devel git nginx -y`
## Firewall
* `sudo apt install ufw -y`
------------
* `sudo systemctl stop firewalld`
* `sudo apt remove --purge firewalld -y`
* `sudo apt remove --purge iptables-persistent netfilter-persistent -y`
------------
* `sudo iptables -P INPUT ACCEPT`
* `sudo iptables -P FORWARD ACCEPT`
* `sudo iptables -P OUTPUT ACCEPT`
* `sudo iptables -F`
* `sudo iptables -X`
* `sudo ip6tables -F`
* `sudo ip6tables -X`
* `if command -v nft > /dev/null; then sudo nft flush ruleset; fi`
------------
* `sudo ufw allow ssh`
* `sudo ufw allow 80,8080,443/tcp`
* `sudo ufw allow 443/udp`
* `sudo ufw enable`
* `sudo ufw status verbose`