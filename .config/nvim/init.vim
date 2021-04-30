call plug#begin("./.nvim/plugins-plug")

Plug 'neovim/nvim-lspconfig'

set spelllang=en,cjk

nnoremap <silent> <F9> :set spell!<cr>
inoremap <silent> <F9> <C-O>:set spell!<cr>
