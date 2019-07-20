all:	fts4

.c.o:
	cc -o $@ $*.c 

fts4:	fts4.o
	ln -o fts4 fts4.o -lc

clean:
	delete #?.o fts4

