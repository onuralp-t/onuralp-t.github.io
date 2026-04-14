* [go back to README](README.md)
# Inside the VM
## before doing other things
* set up 4 GB of swap
```bash
sudo fallocate -l 4G /swapfile && \
sudo chmod 600 /swapfile && \
sudo mkswap /swapfile && \
sudo swapon /swapfile && \
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab && \
sudo sysctl vm.swappiness=5 && \
echo 'vm.swappiness=5' | sudo tee -a /etc/sysctl.conf && \
swapon --show
```
* install common
```bash
sudo apt update -y && \
sudo apt upgrade -y && \
sudo apt install git ufw micro fish build-essential -y && \
sudo chsh -s /usr/bin/fish && \
sudo chsh -s /usr/bin/fish ubuntu # NOTE: assumes username is ubuntu
```
## Firewall migration to 'ufw'
```bash
sudo apt remove --purge firewalld iptables-persistent netfilter-persistent -y && \
sudo ufw limit ssh && \
sudo ufw allow 80,443/tcp && \
sudo ufw allow 80,443/udp && \
sudo iptables -P INPUT ACCEPT && \
sudo iptables -P FORWARD ACCEPT && \
sudo iptables -P OUTPUT ACCEPT && \
sudo iptables -F && \
sudo iptables -X && \
sudo ip6tables -F && \
sudo ip6tables -X && \
sudo ufw --force enable && \
sudo ufw status verbose
```
* To delete rule, run `sudo ufw delete allow 80,443/tcp`.
## Disable plaintext passwords on SSH
* DON'T COPY PASTE
```bash
sudo micro /etc/ssh/sshd_config
# make sure these parameters are set as such:
# PasswordAuthentication no
sudo sshd -t
sudo systemctl restart ssh
```
## fix micro clipboard thing
* * DON'T COPY PASTE
```bash
micro
# inside micro, press CTRL + E
# type `set clipboard terminal`
```
# MISC stuff
## OCI key files permission fix
* In cases where you download ssh keys from Oracle, the file permissions are messed up. If in linux we could've just done `chmod 600 <file>` but Windows requires the following.
* Open an elevated powershell terminal and paste the whole thing, multiline, after changing the `$path`.
```ps1
$path = ".\id_ed25519_bardiel73"
$acl = Get-Acl $path
$acl.SetAccessRuleProtection($true, $false)
$acl.Access | ForEach-Object { $acl.RemoveAccessRule($_) }
$permission = $env:USERNAME, "FullControl", "Allow"
$accessRule = New-Object System.Security.AccessControl.FileSystemAccessRule $permission
$acl.AddAccessRule($accessRule)
Set-Acl $path $acl
```
