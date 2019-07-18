#!/usr/bin/env python

import serial
import os
import sys

CMD_RECEIVE = 23
CMD_SEND    = 24

if len(sys.argv) < 3:
	print ("usage: ftc4.py amiga_file local_file");
	print ("       ftc4.py -s local_file amiga_file");
	sys.exit(1)

cmd = CMD_RECEIVE
if sys.argv[1] == '-s':
	cmd = CMD_SEND
	amigafn = sys.argv[3]
	localfn = sys.argv[2]
else:
	amigafn = sys.argv[1]
	localfn = sys.argv[2]

ser = serial.Serial('/dev/ttyUSB0', 19200,
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_NONE, 
                    stopbits=serial.STOPBITS_ONE,
                    xonxoff=False,
                    rtscts=False,
                    timeout=2)

while True:

	print "handshake..."
	ser.write(bytearray([42]));

	x = ser.read(1);
	print "got: ", repr(x)

	if x == '\x17':
		break;

print "done."

# command + fn len + filename + \0

ser.write(bytearray([cmd, len(amigafn)+1]))
ser.write(amigafn)
ser.write(bytearray([0]))

if cmd == CMD_SEND:
	
	# get size
	l = os.path.getsize(localfn)
	print "file size: %d bytes (%s)" % (l, amigafn)

	with open(localfn, 'rb') as localf:
		total = 0
		while True:

			buf = localf.read(255)
			n = len(buf)

			if n == 0:
				print "finished."
				break

			total += n
			print "sending %d [%8d/%8d] bytes..." % (n, total, l)
			ser.write(bytearray([n]))
			ser.write(buf)
			
			while True:
				x = ser.read(1)
				print "reply: %s" % repr(x)
				if x == '\x17':
					break;
	ser.write(bytearray([0]))
			

else:
	# receive file
	with open(localfn, 'wb') as localf:
		total = 0
		while True:
			x = ser.read(1)
			if len(x) == 0:
				continue
			x = ord(x)
			if x == 0:
				print "done."
				break
			print "reading %3d bytes [total: %9d] ..." % (x, total)
			buf = ser.read(x)
			if len(buf)!=x:
				print "ERR: %d bytes expected, got %d" % (x, len(buf))
			total += x
			ser.write(bytearray([42]))	
			localf.write(buf)


