# FTS4
Amiga FTS4: File Transfer Serial 4

Simple program for file transfer between an Amiga and a client over a serial null modem cable. The protocol used for
file transfer is a subset of the protocol used by Cloanto's Amiga Explorer (AX, for short). 

Related projects:

* lxamiga - Linux/Amiga File Transfer https://github.com/gooofy/lxamiga (this forked version does have error recovery / resync support)
* fuse-aexplorer - A crude re-implementation of the Amiga Explorer protocol, for Linux/FUSE. https://github.com/norly/fuse-aexplorer

## Requirements

* AmigaOS 1.3 or newer, at least 2.0 is recommended

## Usage

```
fts4 
   -v            : increase verbosity
   -b <baudrate> : set serial baudrate, default: 19200
   -D <device>   : serial device, default: serial.device
```

fts4 will keep running until you hit CTRL-C, allowing you to transfer multiple files in one go.

## Example transfer Amiga -> Linux 

on the Linux side:

```bash
./lxamiga.pl -r SYS:foo.txt -w foo.txt
```

## Example transfer Linux -> Amiga

```bash
./lxamiga.pl -s foo.txt SYS:foo.txt
```

## Source Code

Source code is included, to compile the Amiga program you will the Aztec C 5.0a compiler, available here:

http://www.aztecmuseum.ca/compilers.htm#amiga

## TODO

Most of the publically known AX protocol is supported with these limitations:

- 0x01 Transfer cancelled, 0x09 Size?, 0x6c Request size on disk are unsupported for lack of knowledge about protocol (payload) details
- 0x6e/0x0b Format disk (seems a rather odd operation for a remote file protocol)
- 0x6f new directory - seems redundant since 0x66 can create directories as well
- 0x64 when used to list volumes will not list volume names, only devices
- 0x68 will not rename volumes (disks)

## License

Apache 2.0 licensed, code written in 2019 by G. Bartsch

