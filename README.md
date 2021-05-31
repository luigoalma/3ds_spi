# 3ds_spi

Open source implementation of `spi` system module.\
With intent of being a documentation supplement, but also working as replacement.\
Also in mind trying to make result binary as small as possible.

## Note!

This one is still an work in progress, due to some random issues it causes with audio.\
Recommended use only for testing right now.\
I'm currently not seeing the issue causing it, or if it is purely a timing issue.

## Building

Just run `make`.\
It will create a cxi file, and you can extract `code.bin` and `exheader.bin` with `ctrtool`, or some other tool, to place it in `/luma/titles/0004013000002302/`.\
This requires game patching to be enabled on luma config.

## License

This code itself is under Unlicense. Read `LICENSE.txt`\
The folders `source/3ds` and `include/3ds` have some source files that were taken from [ctrulib](https://github.com/smealum/ctrulib), with modifications for the purpose of this program.\
Copy of ctrulib's license in `LICENSE.ctrulib.txt`

## Modifications to ctrulib

Ctrulib changed to generate smaller code, slimmed down sources and headers for a quicker read, and not depend on std libraries.\
As well some changes to behavior on result throw.\
SVCs were also made inlinable by making `static inline` functions that use `__asm__` and explicit `register` variables instead.
