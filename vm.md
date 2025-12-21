* [go back to README](README.md)
# pre-ssh, key file permission issue fix
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
* Change key filename and ephemeral IP and run:
```bash
ssh -i ssh-key-2025-12-21.key ubuntu@141.14d4.232.250
```
# Inside the VM
## before doing other things
```bash
sudo apt update -y && \
sudo apt upgrade -y && \
sudo apt install git nginx ufw nano -y
```
## Firewall migration to 'ufw'
* Copy the whole block and paste it. This removes all firewall managers, installs ufw, enables ports 80, 443 and 8080 for tcp and allows ssh with a ratelimit, then migrates to ufw.
```bash
sudo apt remove --purge firewalld iptables-persistent netfilter-persistent -y && \
sudo ufw limit ssh && \
sudo ufw allow 80,443/tcp && \
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
```
ubuntu@instance-20251221-1816:~$ sudo ufw status
Status: active

To                         Action      From
--                         ------      ----
22/tcp                     LIMIT       Anywhere
80,443/tcp                 ALLOW       Anywhere
22/tcp (v6)                LIMIT       Anywhere (v6)
80,443/tcp (v6)            ALLOW       Anywhere (v6)
```
* To delete rule, run `sudo ufw delete allow 80,443/tcp`.
## git setup
* Add the public key to your Github account via settings.
```bash
ssh-keygen -t ed25519 -C "github/onuralp-t" -f ~/.ssh/id_ed25519
ssh -T git@github.com
```

# Web design
## WASM development
### nginx and WASM
### sqlite3 WASM module
