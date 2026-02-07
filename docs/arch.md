# arch
* wsl --shutdown && wsl ~
* setdefaultuser.sh
```sh
pacman -Syu --noconfirm && pacman -S micro --noconfirm
```
* initarch.sh
```bash
#!/bin/sh
set -x
USER="$USER"
cd ~
pacman -S --noconfirm --needed base-devel git wget zsh tmux wl-clipboard

if id "$USER" &>/dev/null; then
    echo "User already exists."
else
    useradd -m -G wheel -s /bin/bash "$USER"
    echo "$USER:$USER" | chpasswd
    echo "User '$USER' created and password set to '$USER'."
fi

echo "%wheel ALL=(ALL:ALL) NOPASSWD: ALL" > /etc/sudoers.d/temporary_godmode

sudo -u "$USER" bash << EOF
cd /home/$USER
git clone https://aur.archlinux.org/yay-bin.git
cd yay-bin
makepkg -si --noconfirm
cd ..
rm -rf yay-bin
EOF

cat << EOF > /etc/wsl.conf
[user]
default=$USER

[boot]
systemd=true
EOF

chsh -s $(which zsh)


cat << EOFZSHACTUALLY "~/zshsetup.sh"
#!/bin/zsh

set -x

rm "/home/$USER/.zsh*"
rm "/home/$USER/.bash*"
mkdir -p /home/$USER/.config/zsh

cat << EOFZSHRC > /home/$USER/.config/zsh/.zshrc
mkdir -p "\$ZDOTDIR/cache"
zstyle ':completion:*' cache-path "\$ZDOTDIR/cache"

autoload -Uz compinit
compinit -d "\$ZDOTDIR/.zcompdump"
EOFZSHRC

cat << EOFENV > /etc/zsh/zshenv
export ZDOTDIR="/home/$USER/.config/zsh
EOFENV

EOFZSHACTUALLY


sudo -u "$USER" zsh << EOF
cd /home/$USER
git clone https://aur.archlinux.org/yay-bin.git
cd yay-bin
makepkg -si --noconfirm
cd ..
rm -rf yay-bin
EOF



```