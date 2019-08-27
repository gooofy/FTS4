#include <exec/exec.h>
#include <functions.h>
#include <devices/serial.h>
#include <libraries/dosextens.h>
#include <stdio.h>
#include <stdarg.h>

#include "crc.h"

#define VERSION "0.1.0"

#define DEFAULT_BAUDRATE  19200
#define DEFAULT_DEVICE    "serial.device"

static ULONG baudrate    = DEFAULT_BAUDRATE;
static char *device_name = DEFAULT_DEVICE;

#define BUFSIZE    1024
#define READSIZE    512
#define PATH_MAX    512

#define LOG_DEBUG2    0
#define LOG_DEBUG     1
#define LOG_INFO      2
#define LOG_ERROR     3

static int loglevel = LOG_INFO;

#define MSG_NEXT_PART  0x0000
#define MSG_INIT       0x0002
#define MSG_MPARTH     0x0003
#define MSG_EOF        0x0004
#define MSG_BLOCK      0x0005
 
#define MSG_EXISTS     0x0008
#define MSG_ACK_CLOSE  0x000a

#define MSG_FILE_SEND  0x0065
#define MSG_FILE_RECV  0x0066
#define MSG_FILE_CLOSE 0x006d

static ULONG              wait_mask;
static struct MsgPort    *mp_serial   = NULL;
static struct IOExtSer   *io_serial   = NULL;
static BOOL               serial_open = FALSE;
static struct FileHandle *io_file     = NULL;

static void log(int level, char *msg, ...)
{
   va_list argp;

   if (level < loglevel)
      return;

   va_start(argp, msg);
   vprintf(msg, argp);
   va_end(argp);
   fflush(stdout);
}


static void closedown(void)
{
   log(LOG_DEBUG, "closedown.\n");
   if (serial_open)
   {
      AbortIO((struct IORequest*)io_serial);
      WaitIO((struct IORequest*)io_serial);
      CloseDevice((struct IORequest*)io_serial);
   }
   if (io_serial)
   {
      DeleteExtIO( (struct IORequest *) io_serial);
   }
   if (mp_serial)
      DeletePort(mp_serial);
   if (io_file)
      Close((BPTR) io_file);
   log(LOG_INFO, "goodbye.\n");
   exit();
}

static void print_usage(char *myname)
{
   printf ("usage: %s [options]\n", myname);
   printf ("   -v            : increase verbosity\n");
   printf ("   -b <baudrate> : set serial baudrate, default: %d\n", 
           DEFAULT_BAUDRATE);
   printf ("   -D <device>   : serial device, default: %s\n", 
           DEFAULT_DEVICE);
   closedown();
}

static void parse_commandline(int argc, char **argv)
{
   int i=1;
   while (i<argc)
   {
      if (!strcmp(argv[i], "-v"))
      {
         loglevel--;
         i++;
      } 
      else if (!strcmp(argv[i], "-b"))
      {
         i++;
         if (i>=argc)
            print_usage(argv[0]);
         baudrate = atoi(argv[i]);
         i++;   
      }
      else if (!strcmp(argv[i], "-D"))
      {
         i++;
         if (i>=argc)
            print_usage(argv[0]);
         device_name = argv[i];
         i++;   
      }
      else
         print_usage(argv[0]);
   }
}

static void setup_serial(ULONG baud)
{
   log (LOG_INFO, "setting baudrate to %d\n", baud);
   io_serial->IOSer.io_Command  = SDCMD_SETPARAMS;
   io_serial->io_Baud           = baud ;
   io_serial->io_RBufLen        = BUFSIZE;
   if (DoIO( (struct IORequest*) io_serial))
   {
      log (LOG_ERROR, "*** ERROR: failed to set serial parameters!\n");
      closedown();
   }
}

static void read_serial(int len, UBYTE *buf)
{
   ULONG signals;

   log(LOG_DEBUG2, "reading %d bytes from serial port...\n", len);
   io_serial->IOSer.io_Command = CMD_READ;
   io_serial->IOSer.io_Length  = len;
   io_serial->IOSer.io_Data    = (APTR)buf;
   SendIO( (struct IORequest*) io_serial);

   while ( 1 )
   {
      signals = Wait(wait_mask);

      /* CTRL-C ? */
      if (signals & SIGBREAKF_CTRL_C)
      {
         log(LOG_INFO, "CTRL-C detected, aborting.\n");
         closedown(); 
      }

      if (CheckIO((struct IORequest*) io_serial))
      {
         int len_actual, i;
         WaitIO((struct IORequest*)io_serial);
         len_actual = io_serial->IOSer.io_Actual;
         log(LOG_DEBUG2, "%ld bytes received:", 
                len_actual);
         for (i=0; (i<len_actual && i<9); i++)
            log(LOG_DEBUG2, " %02x", buf[i]);
         log(LOG_DEBUG2, "\n");
         if (len_actual != len) 
         {
            log (LOG_ERROR, 
                 "*** ERROR: got %d bytes from serial port, expected %d\n",
                 len_actual, len);
            closedown();
         }
         break;
      }
   }
}

static void write_serial(int len, UBYTE *buf)
{
   ULONG signals;

   log (LOG_DEBUG2, "sending %d bytes to serial port...\n", len);
   io_serial->IOSer.io_Command = CMD_WRITE;
   io_serial->IOSer.io_Length  = len;
   io_serial->IOSer.io_Data    = (APTR)buf;
   SendIO( (struct IORequest*) io_serial);

   while ( 1 )
   {
      signals = Wait(wait_mask);

      /* CTRL-C ? */
      if (signals & SIGBREAKF_CTRL_C)
      {
         log (LOG_INFO, "CTRL-C detected, aborting.\n");
         closedown(); 
      }

      if (CheckIO((struct IORequest*) io_serial))
      {
         int len_actual;
         WaitIO((struct IORequest*)io_serial);
         len_actual = io_serial->IOSer.io_Actual;
         log (LOG_DEBUG2, "%ld bytes sent: %02x%02x%02x%02x.\n", 
              len_actual,
              buf[0], buf[1], buf[2],
              buf[3]);
         if (len_actual != len) 
         {
            log (LOG_ERROR, 
                 "*** ERROR: sent %d bytes to serial port, expected %d\n",
                 len_actual, len);
            closedown();
         }
         break;
      }
   }
}

static void write_ack(void)
{
   write_serial(4, (UBYTE*) "PkOk");
}

static void write_nack(void)
{
   write_serial(4, (UBYTE*) "PkEr");
}

struct ax_header {
   WORD msg;
   WORD len;
   ULONG seq;
   ULONG crc;
} ;

static void read_message(struct ax_header *header, UBYTE *payload, int max_len)
{
   ULONG crc2;

   /* header */

   read_serial(12, (UBYTE *) header);
   crc2 = crc32((UBYTE *) header, 8);

   log (LOG_DEBUG, "MSG : cmd=0x%04x len=%d seq=%d crc=%08x crc2=%08x\n",
        header->msg, header->len, header->seq, header->crc, crc2);

   if (header->crc != crc2) 
   {
      log (LOG_ERROR, "ERROR: message header CRC mismatch!\n");
      write_nack();
      closedown();
   }
   /* FIXME: check sequence! */

   /* payload, if any */

   if (header->len)
   {
      ULONG crc1;
      if (header->len > max_len)
      {
         log (LOG_ERROR, "ERROR: buffer overflow (%d > %d)\n", 
              header->len, max_len);
         closedown();
      }
      read_serial(header->len, payload);
      read_serial(4, (UBYTE *) &crc1);
      crc2 = crc32(payload, header->len);
      if (crc1 != crc2)
      {
         log (LOG_ERROR, "ERROR: crc mismatch in payload data: %08x vs %08x\n",
              crc1, crc2);
         write_nack();
         closedown();
      }
   }
   write_ack();
}

static void read_ack(void)
{
   ULONG ack;
   read_serial(4, (UBYTE*) &ack);
   if (ack != 0x506b4f6b /* PkOk */)
   {
      log (LOG_ERROR, "*** ERROR: read_ack failed!\n");
      closedown();
   }
}

static void write_message(WORD msg, UBYTE *payload, int len)
{
   static ULONG seq = 0;
   struct ax_header header;

   header.msg = msg;
   header.len = len;
   header.seq = seq++;
   header.crc = crc32((UBYTE*)&header, 8);

   log (LOG_DEBUG, "WMSG: cmd=0x%04x len=%d seq=%d crc=%08x\n",
        header.msg, header.len, header.seq, header.crc);

   write_serial(12, (UBYTE*) &header);

   /* payload, if any */
   if (len)
   {
      ULONG crc1;
      write_serial(len, payload);
      crc1 = crc32(payload, len);
      write_serial(4, (UBYTE*) &crc1);
   }

   read_ack();
}

struct ax_recv {
   ULONG len;
   ULONG file_size;
   ULONG unknown;
   ULONG attrs;
   ULONG date;
   ULONG time;
   ULONG ctime;
   UBYTE unknown2;
};

static struct ax_recv recv;
static char           filename[PATH_MAX];
static ULONG          receiving=0, received;
static ULONG          sending=0, sent;

static void msg_recv (UBYTE *recv_buf, WORD recv_len)
{
   struct Lock     *file_lock;
   struct InfoData *file_info;

   recv = *( (struct ax_recv *)recv_buf );
   strncpy (filename, (char *)recv_buf+29, PATH_MAX);
   filename[PATH_MAX-1] = 0;

   log(LOG_DEBUG, "msg_recv %s size=%d attrs=0x%08x date=%d time=%d ctime=%d len=%d\n",
       filename, recv.file_size,
       recv.attrs, recv.date, recv.time, recv.ctime, recv.len);

   /* does this file exist ? */

   file_lock = (struct Lock *) Lock (filename, ACCESS_READ);
   if (file_lock)
   {
      write_message (MSG_EXISTS, NULL, 0);
      UnLock ((BPTR)file_lock);
   } 
   else
   {    
      write_message (MSG_NEXT_PART, NULL, 0);
   }
}

static void msg_mparth (UBYTE *buf, WORD len)
{
   ULONG flags = *( ((ULONG*) buf) + 1 ); /* FIXME: implement */

   receiving = *( (ULONG*) buf );
   received  = 0;
   sending   = 0;

   log(LOG_DEBUG, "msg_mparth receiving=%d, flags=%0x08x\n", receiving, flags);

   if (io_file)
      Close((BPTR)io_file);

   /* FIXME: attributes, mode!, timestamps */
   io_file = (struct FileHandle *) Open(filename, MODE_NEWFILE);
   if (!io_file)
   {
      log(LOG_ERROR, "*** ERROR: couldn´t open file for writing: %s\n",
          filename);
      write_message(MSG_EXISTS, NULL, 0);
      return;
   }

   write_message(MSG_NEXT_PART, NULL, 0);
}

static void msg_block (UBYTE *buf, WORD len)
{
   ULONG pos   = *( (ULONG*) buf );

   if (receiving)
   {
      received += len-4;

      log(LOG_DEBUG, "msg_block recv pos=%d, %d/%d\n", pos, received, receiving);

      Seek((BPTR)io_file, pos, OFFSET_BEGINNING);
      Write((BPTR)io_file, (char*) &buf[4], len-4);

      write_message(MSG_NEXT_PART, NULL, 0);
   }
   else
   {
      log(LOG_ERROR, "*** ERROR: MSG_BLOCK received while not sending!\n");
      closedown();
   }
}

static void msg_eof (UBYTE *buf, WORD len)
{
   log(LOG_DEBUG, "msg_eof\n");
   receiving = 0;
   sending   = 0;
}

static void msg_file_send (UBYTE *buf, WORD len)
{
   strncpy (filename, (char *)buf, PATH_MAX);
   filename[PATH_MAX-1] = 0;

   log(LOG_DEBUG, "msg_file_send %s\n", filename);

   if (io_file)
      Close((BPTR)io_file);

   io_file = (struct FileHandle *) Open(filename, MODE_OLDFILE);
   if (!io_file)
   {
      log(LOG_ERROR, "*** ERROR: couldn´t open file for reading: %s\n",
          filename);
      write_message(MSG_EXISTS, NULL, 0);
      return;
   }

   receiving = 0;
   received  = 0;

   /* determine file size */
   Seek((BPTR)io_file, 0, OFFSET_END);
   sending = Seek((BPTR)io_file, 0, OFFSET_BEGINNING);
   sent    = 0;

   log (LOG_DEBUG, "msg_file_send: file size is %d bytes.\n", sending);
   write_message(MSG_MPARTH, (UBYTE*) &sending, 4);
}

static void msg_next_part (UBYTE *buf, WORD len)
{
   ULONG pos   = *( (ULONG*) buf );

   if (sending)
   {
      ULONG l;
      sent = Seek((BPTR)io_file, 0, OFFSET_CURRENT);
      l = Read((BPTR)io_file, (char*) &buf[4], READSIZE);

      log(LOG_DEBUG, "msg_next_part send %d/%d\n", sent, sending);

      if (l>0)
      {
         *((ULONG*)buf) = sent;
	 write_message(MSG_BLOCK, buf, l+4);
      }
      else
      {
	 write_message(MSG_EOF, NULL, 0);
      }
   }
   else
   {
      log(LOG_DEBUG, "SYNC\n");
   }
}
int main(int argc, char **argv)
{
   UBYTE buf_serial[BUFSIZE];
   ULONG signals;
   struct ax_header header;

   log (LOG_INFO, "FTS4 %s (C) 2019 by G. Bartsch\n\n", VERSION);

   parse_commandline(argc, argv);

   log (LOG_INFO, "Opening %s ...\n", device_name);

   mp_serial = CreatePort(0,0);
   if (!mp_serial)
   {
      printf("ERROR: cannot create port.\n");
      closedown();
   }

   io_serial = (struct IOExtSer *) CreateExtIO(mp_serial, sizeof(struct IOExtSer)) ;
   if (!io_serial)
   {
      log (LOG_ERROR, "ERROR: cannot create IOExtSer.\n");
      closedown();
   }

   io_serial->io_SerFlags = SERF_7WIRE | SERF_SHARED | SERF_XDISABLED;
   serial_open = !OpenDevice(device_name, 0, (struct IORequest*)io_serial, 0);
   if (!serial_open)
   {
      log (LOG_ERROR, "ERROR: %s did not open.\n", device_name);
      closedown();
   }

   wait_mask = SIGBREAKF_CTRL_C | 1L << mp_serial->mp_SigBit;

   setup_serial(baudrate);

   while (TRUE)
   {
      read_message(&header, buf_serial, BUFSIZE);

      switch (header.msg) 
      {
         case MSG_INIT:
            write_message(MSG_INIT, (UBYTE *) "Cloanto", 7);
            break;

         case MSG_FILE_RECV:
            msg_recv(buf_serial, header.len);
            break;

         case MSG_MPARTH:
            msg_mparth(buf_serial, header.len);
            break;

         case MSG_BLOCK:
            msg_block(buf_serial, header.len);
            break;

         case MSG_EOF:
            msg_eof(buf_serial, header.len);
            break;

         case MSG_FILE_CLOSE:
            if (io_file)
               Close((BPTR) io_file);
            io_file = NULL;
            write_message(MSG_ACK_CLOSE, NULL, 0);
            break;

         case MSG_FILE_SEND:
            msg_file_send(buf_serial, header.len);
            break;

         case MSG_NEXT_PART:
            msg_next_part(buf_serial, header.len);
            break;

         default:
            log (LOG_ERROR, "*** ERROR: unknown message 0x%04x received!\n",
                 header.msg);
            closedown();
      }
   }
}

