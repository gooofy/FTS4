# FTS4
Amiga FTS4: File Transfer Serial 4

Simple program for file transfer between an Amiga and a client over a serial null modem cable. The protocol used for
file transfer is a (currently rather small) subset of the protocol used by Cloanto's Amiga Explorer (AX, for short). 

The aim for this project is to become fully AX compliant one day.

Related projects:

* lxamiga - Linux/Amiga File Transfer https://github.com/marksmanuk/lxamiga
* fuse-aexplorer - A crude re-implementation of the Amiga Explorer protocol, for Linux/FUSE. https://github.com/norly/fuse-aexplorer

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

## License

Apache 2.0 licensed, code written in 2019 by G. Bartsch

