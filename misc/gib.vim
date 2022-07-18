" Vim syntax file

if exists("b:current_syntax")
  finish
endif

if !exists('main_syntax')
  let main_syntax = 'gib'
endif

syn sync minlines=10
" syn case ignore

syn match Comment /^#.*/
syn match gibvaruse "$([^)]\+)"
syn match gibvarset /^\(let\*\?\|set\*\?\|add\*\?\) \+\([a-z]\+\)/ contains=gibvarname
syn match gibvarcmd /^src \+\([a-z]\+\) \+\([a-z]\+\)/ contains=gibvarname
syn match gibvarcmd /^\(for\|def\|use\) \+\([a-z]\+\)/ contains=gibvarname

syn match gibcmd /^sub?\?/
syn match gibcmd /^cmd \+\([a-z_/.$()-]\+\)/ contains=gibcmdpath

syn match gibvarname / [a-z_.-]\+/ contained
" syn match gibcmdpath / [a-z_/.-]\+/ contained

syn keyword gibdepst
      \ dep
      \ out
      \ meta

hi def gibvaruse ctermfg=green
hi def gibvarset ctermfg=cyan
hi def gibdepst  ctermfg=red

hi def gibmacst  ctermfg=magenta
hi def gibcmd    ctermfg=magenta
hi def gibvarcmd ctermfg=magenta

hi def gibvarname ctermfg=yellow
hi def gibcmdpath ctermfg=yellow

hi def muBold     term=italic cterm=italic gui=italic
hi def muCode     term=italic ctermfg=Green gui=italic

let b:current_syntax = "gib"
if main_syntax ==# 'gib'
  unlet main_syntax
endif

" vim:set sw=2:
