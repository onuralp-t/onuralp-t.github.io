```sh
wsl --shutdown
wsl --unregister archlinux
#-----------------------------------------------------------------------
wsl --export archlinux - | 7z a -si %UserProfile%\Desktop\arch.tar.7z -mx=9 -m0=lzma2 -md=256m -mfb=64 -ms=16g -mmt=8
7z x -so "%UserProfile%\Desktop\wsl_arch_syu.7z" | wsl --import Arch "C:\Users\bard\AppData\Local\wsl_storage" -
```
* before script run:
```sh
pacman -Syu --noconfirm && pacman -S --noconfirm zsh micro xclip
```
* main script:
```zsh
set -x
USER="$USER"
# -----------------------------------------------------------------------INSTALL COMMON
pacman -S --noconfirm --needed base-devel git wget tmux
# -----------------------------------------------------------------------CREATE USER
if id "$USER" &>/dev/null; then
    echo "User already exists."
else
    useradd -m -G wheel -s /bin/bash "$USER"
    echo "$USER:$USER" | chpasswd
    echo "User '$USER' created and password set to '$USER'."
fi
# -----------------------------------------------------------------------ENABLE GODMODE
echo "%wheel ALL=(ALL:ALL) NOPASSWD: ALL" > /etc/sudoers.d/temporary_godmode
# -----------------------------------------------------------------------SUDO STUFF
echo "export ZDOTDIR=/home/$USER/.config/zsh" > /etc/zsh/zshenv
chsh -s $(which zsh)

cat << SETUP_WSL > /etc/wsl.conf
[user]
default=$USER

[boot]
systemd=true
SETUP_WSL
# -----------------------------------------------------------------------DISABLE GODMODE
rm /etc/sudoers.d/temporary_godmode
echo "%wheel ALL=(ALL:ALL) ALL" > /etc/sudoers.d/permanent
# -----------------------------------------------------------------------SETUP MICRO
mkdir -p /home/$USER/.config/micro
cat << SETUP_MICRO > /home/$USER/.config/micro/settings.json
{
    "clipboard": "external"
}
SETUP_MICRO
# -----------------------------------------------------------------------SETUP ZSH
touch /home/$USER/.zshrc
sudo -u "$USER" zsh << SETUP_ZSH

rm "/home/$USER/.zsh*"
rm "/home/$USER/.bash*"
mkdir -p /home/$USER/.config/zsh

cat << EOFZSHRC > /home/$USER/.config/zsh/.zshrc
mkdir -p "\$ZDOTDIR/cache"
zstyle ':completion:*' cache-path "\$ZDOTDIR/cache"

autoload -Uz compinit
compinit -d "\$ZDOTDIR/.zcompdump"
EOFZSHRC

SETUP_ZSH
# -----------------------------------------------------------------------SETUP YAY
sudo -u "$USER" zsh << SETUP_YAY
cd /home/$USER
git clone https://aur.archlinux.org/yay-bin.git
cd yay-bin
makepkg -si --noconfirm
cd ..
rm -rf yay-bin
SETUP_YAY
```