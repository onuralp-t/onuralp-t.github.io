# Bootstrap a VPS
# Choosing a Cloud Compute provider
Choose Oracle's free-tier VPS. They offer 1 vCPU, 1 GB RAM, 200 GB SSD and a public IP for free, though it's not static IP at free tier. This is enough to host a website for your personal project.

They also have an `Always Free` indicator that can assure you that you won't get charged if you stay in the always free tier. Account validation is a bit cumbersome as they ask for bunch of PII and also ask you to pay a nominal fee which they send you back as a way of identity verification.

- Google Cloud Compute used to be my choice, but they recently stopped giving away free public IP's, so hosting small web servers on free-tier Google VPS's became no longer possible. Also not being able to put payment limits was always annoying.

# Meta settings for the Cloud Compute Provider
- Network: As a general rule, you should start with only allowing network ingress/egress traffic to your VPS through standard HTTP(S) ports 80 and 443. You should set this up at the cloud-layer through your VPS's subnet settings, and we will set up these firewall rules inside the VPS itself as well.
- SSH/keys: When setting up the network portion of your VPS in OCI, you get the option to have OCI make the SSH keys for you, and download the SSH keys to your machine. This allows you to connect to your VPS via your SSH keys, foregoing more insecure plaintext password authentication schemes. A small note, when you download the keys from OCI, you need to set up their permissions correctly.

# Inside the VPS
If you try to do a full system upgrade like `sudo apt update -y && sudo apt upgrade -y`, you will most likely stall to an almost complete stop and proceed very slowly because of the inadequacies of 1 GB of system RAM. Setting up swap memory and then update/upgrading is usually a solid first start.

## Setting up swap and Upgrading
Create 4GB of swap file, fix permissions, make swap, turn swap on, adjust swappiness for low RAM system.
```sh
sudo fallocate -l 4G /swapfile && \
sudo chmod 600 /swapfile && \
sudo mkswap /swapfile && \
sudo swapon /swapfile && \
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab && \
sudo sysctl vm.swappiness=5 && \
echo 'vm.swappiness=5' | sudo tee -a /etc/sysctl.conf && \
sudo apt update -y && sudo apt upgrade -y && sudo apt autoremove -y && sudo apt install build-essential git micro ufw -y
```

## Migrate firewall to UFW
UFW has a simple and easy to use interface, and it's my preference for configuring firewall inside a Linux system, though these commands below are easily adaptable to other systems.

A common beginner mishap is improperly migrating to a different firewall system and blocking yourself out of your VPS by disallowing foreign connections (including your own connection). To mitigate this, directly copy-pasting the below command so every command runs sequentially is advised. (see: `sudo ufw limit ssh`)
```sh
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
- To delete a rule, you can run `sudo ufw delete allow 80,443/tcp`

## Disable plaintext password SSH connection attempts
Don't set up an easy password thinking nobody would bother touching your site. All public websites get malicious traffic, and you have to expect and defend against them. Disabling plaintext password makes it so you can only enable an SSH connection to your VPS
```sh
sudo micro /etc/ssh/sshd_config
# make sure these parameters are set as such:
# PasswordAuthentication no
sudo sshd -t
sudo systemctl restart ssh
```

## Set up Git
When setting up git, there are these 6 - 7 commands that I have to look up whenever I'm in a fresh environment, so here they are.
```sh
ssh-keygen -t ed25519 && \
git config --global gpg.format ssh && \
git config --global user.signingkey "~/.ssh/id_ed25519.pub" && \
git config --global commit.gpgsign true && \
git config --global user.email "149798217+onuralp-t@users.noreply.github.com" && \
git config --global user.name "onuralp-t" && \
echo "$(git config --global user.email) $(cat ~/.ssh/id_ed25519.pub)" >> ~/.ssh/allowed_signers
```

## Optional: pretty prompt
I like pretty prompts, and default bash prompt is pretty ugly and unergonomic for non-experts. I tried zsh and fish but didn't like them much. So I decided to just prettify the existing bash setup and add some functionality. This is a basic setup that I like:
```sh
sudo apt install -y fzf bash-completion && \
curl -sS https://starship.rs/install.sh | sh
```
```sh
cat << 'EOF' > ~/.inputrc
set completion-ignore-case On
set show-all-if-ambiguous On
set completion-map-case On
set colored-stats On
TAB: menu-complete
# Shift-Tab to cycle backwards
"\e[Z": menu-complete-backward
EOF
```
```sh
cat << 'EOF' > ~/.config/starship.toml
add_newline = false
[line_break]
disabled = true

[character]
success_symbol = "[❯](bold green)"
error_symbol = "[❯](bold red)"

[directory]
truncation_length = 3
truncate_to_repo = true
style = "bold cyan"

[git_branch]
format = "[$branch]($style) "
style = "bold purple"

[git_status]
format = '([$all_status$ahead_behind]($style) )'
style = "red"
EOF
```
```sh
cat << 'EOF' > ~/.bashrc
# If not running interactively, don't do anything
[[ $- != *i* ]] && return

alias ls='ls --color=auto'
alias grep='grep --color=auto'

if [ -f /etc/bash_completion ]; then
    . /etc/bash_completion
elif [ -f /usr/share/bash-completion/bash_completion ]; then
    . /usr/share/bash-completion/bash_completion
fi

if command -v fzf &>/dev/null; then
    eval "$(fzf --bash)"
fi

eval "$(starship init bash)"
EOF
```
