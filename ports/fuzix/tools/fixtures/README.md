# C16 V2 migration fixture

`c16-v2.img.gz` is a gzip-compressed 2,048-block image generated using the
committed V2 `rootImage` implementation from revision
`b94ea2a809f85038a00ad0fcb59aadc215018e8b` (HEAD when V3 work began).
It contains `/init`, `/note` with cells `abcd 1234 000a` and mode 0640,
`/bin/large` containing 1,100,000 cells of `9abc`, hard link `/bin/alias`, and
`/dev/test` with device number `0123`. The large file uses V2's double-indirect
tree. The fixture tests the real previous disk format, rather than serializing
V2 through the new formatter. It contains no user disk data.
