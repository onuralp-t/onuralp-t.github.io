* [go back to README](README.md)
# pre-ssh
* In cases where you download ssh keys from Oracle, the file permissions are messed up. If in linux we could've just done `chmod 600 <file>` but Windows requires the following.
* Open an elevated powershell terminal and paste the whole thing, multiline, after changing the `$path`.
```ps1
$path = ".\ssh-key-2025-12-21.key"
$acl = Get-Acl $path
$acl.SetAccessRuleProtection($true, $false)
$acl.Access | ForEach-Object { $acl.RemoveAccessRule($_) }
$permission = $env:USERNAME, "FullControl", "Allow"
$accessRule = New-Object System.Security.AccessControl.FileSystemAccessRule $permission
$acl.AddAccessRule($accessRule)
Set-Acl $path $acl
```
# Basics
```bash
sudo apt update -y && \
sudo apt upgrade -y && \
sudo apt install git nginx ufw nano -y
```
## Firewall
* Copy the whole block and paste it. This removes all firewall managers, installs ufw, enables ports 80, 443 and 8080 for tcp and allows ssh with a ratelimit, then migrates to ufw.
```bash
sudo apt remove --purge firewalld iptables-persistent netfilter-persistent -y && \
sudo ufw limit ssh && \
sudo ufw allow 80,8080,443/tcp && \
sudo iptables -P INPUT ACCEPT && \
sudo iptables -P FORWARD ACCEPT && \
sudo iptables -P OUTPUT ACCEPT && \
sudo iptables -F && \
sudo iptables -X && \
sudo ip6tables -F && \
sudo ip6tables -X && \
(if command -v nft > /dev/null; then sudo nft flush ruleset; fi) && \
sudo ufw --force enable && \
sudo ufw status verbose
```