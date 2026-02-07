# WSL
```zsh
wsl --install Fedora-43
wsl ~
```
# Fedora-43
```zsh
sudo dnf update -y && sudo dnf upgrade -y
sudo dnf install ncurses build-essential cmake gdb clang lld valgrind mingw-w64 zsh micro git -y --skip-unavailable
```
# set passwd for user (necessary)
```zsh
sudo passwd bard             # 8 char long password not too simple
sudo usermod -aG wheel bard
```
# zsh
## make zsh default terminal
```zsh
sudo chsh -s /bin/zsh    # chsh -l to see paths
```
## set prompt in /etc/zshrc
```zsh
micro /etc/zshrc
####### in file, towards the top, overwrite(delete) preexisting PROMPT:
autoload -Uz vcs_info
zstyle ':vcs_info:git:*' formats ' %F{yellow}(%b)%f'
precmd() {
    vcs_info
    local user='%F{green}%n%f'
    local location='%F{cyan}%~%f'
    local symbol='%F{magenta}$%f'
    PROMPT="${user}:${location}${vcs_info_msg_0_} ${symbol} "
}

autoload -Uz compinit
compinit -u

HISTSIZE=5000
SAVEHIST=5000
setopt appendhistory
setopt sharehistory
setopt hist_ignore_dups

# alias grep='grep --color=auto'
# alias wall='clang -Wall -Wextra -Wpedantic -std=c11'
# source /usr/share/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh
```