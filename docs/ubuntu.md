# L inux
## wsl
* install with `wsl --list --online`, `wsl --install Ubuntu`
* run `wsl ~` for it to open in your home dir
* prefer Terminal over cmd.exe
## install common
* `sudo apt update -y` and `sudo apt upgrade -y`
* `sudo apt install build-essential cmake gdb clang lld valgrind mingw-w64 zsh micro git -y`
## create ~/.config dir
* `mkdir ~/.config`
## zsh
* make zsh default with `chsh -s $(which zsh)`
### setup ZDOTDIR
* open ` micro /etc/zsh/zshenv`
* add following to outside of any conditional blocks, at top of file
```sh
export ZDOTDIR=$HOME/.config/zsh
```
### .zshrc
* `mkdir ~/.config/zsh` and ` ~/.config/zsh/.zshrc` 
```zsh
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
## Gitting hub
