# FTS4
Amiga FTS4: File Transfer Serial, Version 4

This is a very simple pair of programs for file transfer between an Amiga and a Linux computer over a serial null modem cable.

Serial port setting:
* 19200 bps
* 8N1
* no flow control

This tool will transfer a single file only per run, to transfer multiple files or directories, pack them first (e.g. via lha).

## Amiga -> Linux 

on the Linux side:

```bash
./ftc4.py 'RAM:foo.lha' 'foo.lha'
```

on the Amiga side:

```bash
fts4
```

## Linux -> Amiga

```bash
./ftc4.py -s foo.lha 'RAM:foo.lha'
```

on the Amiga side:

```bash
fts4
```

## Source Code

Source code is included, to compile the Amiga program you will the Aztec C 5.0a compiler, available here:

http://www.aztecmuseum.ca/compilers.htm#amiga

## License

Apache 2.0 licensed, code written in 2019 by G. Bartsch


