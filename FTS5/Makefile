all:	fts5

.c.o:
	cc -o $@ $*.c 

fts5:	fts5.o crc.o
	ln -o fts5 fts5.o crc.o -lc

clean:
	delete #?.o fts5

