PS1="%F{cyan}[%n@%m]%f %F{red}::%f %F{green}%~%f %F{magenta}%#%f " 

#History Settings
HISTORY="$HOME/.zsh_history"
HISTSIZE=5000
SAVEHIST=1000
setopt HIST_EXPIRE_DUPS_FIRST
setopt HIST_IGNORE_DUPS
setopt HIST_IGNORE_SPACE
setopt SHARE_HISTORY

#Misc Options
setopt AUTOCD
setopt CORRECT
setopt AUTO_MENU

#Plugins
source /usr/share/zsh/plugins/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh
source /usr/share/zsh/plugins/zsh-autosuggestions/zsh-autosuggestions.zsh

#Aliases
alias ls="exa"
alias la="exa -a"
alias ll="exa -l"
alias lla="exa -a -l"
alias emacs="emacs -nw"
alias esync="~/.emacs.d/bin/doom sync"
alias inst="sudo pacman -S"

#Autocomplete
#The following lines were added by compinstall
zstyle ':completion:*' completer _expand _complete _ignored _correct _approximate
zstyle ':completion:*' list-colors ${(s.:.)LS_COLORS}
zstyle ':completion:*' list-prompt %SAt %p: Hit TAB for more, or the character to insert%s
zstyle ':completion:*' matcher-list '' 'm:{[:lower:][:upper:]}={[:upper:][:lower:]}' 'r:|[._-]=* r:|=*'
zstyle ':completion:*' menu select=1
zstyle ':completion:*' select-prompt %SScrolling active: current selection at %p%s
zstyle :compinstall filename '/home/luna/.zshrc'

autoload -Uz compinit
compinit
#End of lines added by compinstall

#Variables
PAGER=most
