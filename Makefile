all:	fts4

.c.o:
	cc -so -o $@ $*.c 

fts4:	fts4.o crc.o
	ln -o fts4 fts4.o crc.o -lc

clean:
	delete #?.o fts4

