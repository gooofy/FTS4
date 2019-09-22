/*
 * FTS4 - serial file transfer
 *
 * Copyright 2019 G. Bartsch
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <exec/exec.h>
#include <functions.h>
#include <devices/serial.h>
#include <devices/timer.h>
#include <libraries/dosextens.h>
#include <stdio.h>
#include <stdarg.h>

extern struct DosLibrary *DOSBase;

/* V36 stuff */
extern BOOL SetFileDate(const char *name, struct DateStamp *date);
#pragma amicall(DOSBase,0x18c, SetFileDate(d1,d2));

#include "crc.h"

#define VERSION "0.3.2"

/* #define DEBUG_BYTES */

#define DEFAULT_BAUDRATE  19200
#define DEFAULT_DEVICE    "serial.device"

static ULONG baudrate    = DEFAULT_BAUDRATE;
static char *device_name = DEFAULT_DEVICE;

#define BUFSIZE      1024
#define READSIZE      512
#define PATH_MAX      512
#define DIRBUF_SIZE 16384

#define SERIAL_TIMEOUT_SECS  1
#define SERIAL_TIMEOUT_MIRCO 0

#define LOG_DEBUG2    0
#define LOG_DEBUG     1
#define LOG_INFO      2
#define LOG_ERROR     3

static int loglevel = LOG_INFO;

#define MSG_NEXT_PART   0x00
#define MSG_INIT        0x02
#define MSG_MPARTH      0x03
#define MSG_EOF         0x04
#define MSG_BLOCK       0x05
 
#define MSG_IOERR       0x08
#define MSG_ACK_CLOSE   0x0a

#define MSG_DIR         0x64
#define MSG_FILE_SEND   0x65
#define MSG_FILE_RECV   0x66
#define MSG_FILE_DELETE 0x67
#define MSG_FILE_RENAME 0x68
#define MSG_FILE_MOVE   0x69
#define MSG_FILE_COPY   0x6a
#define MSG_FILE_ATTR   0x6b
#define MSG_FILE_CLOSE  0x6d

struct ax_header 
{
   UBYTE sync;
   UBYTE msg;
   WORD  len;
   ULONG seq;
   ULONG crc;
} ;

#define AX_FILE_TYPE_DIR  2
#define AX_FILE_TYPE_FILE 3

struct ax_recv 
{
   ULONG len;
   ULONG file_size;
   ULONG unknown;
   ULONG attrs;
   ULONG date;
   ULONG time;
   ULONG ctime;
   UBYTE file_type;
};

struct ax_dirent 
{
   ULONG len;    
   ULONG size;
   ULONG used;
   WORD  type;
   WORD  attrs; 
   ULONG date;  
   ULONG time;  
   ULONG ctime; 
   UBYTE type2; 
};

static ULONG                 wait_mask;
static struct MsgPort       *mp_serial   = NULL;
static struct IOExtSer      *io_serial   = NULL;
static BOOL                  serial_open = FALSE;

static struct timerequest    io_tr;
static BOOL                  timer_open  = FALSE;

static struct FileHandle    *io_file     = NULL;
static ULONG                 io_flags=0;
static struct ax_recv        recv;
static char                  filename[PATH_MAX];
static char                  newname[PATH_MAX];
static ULONG                 receiving=0, received;
static ULONG                 sending=0, sent;
static struct Lock          *lock = NULL;
static struct FileInfoBlock *fib = NULL;
static char                 *dirbuf = NULL;
static ULONG                 dirbuf_todo = 0, dirbuf_done = 0;
static BOOL                  dirbuf_sending = FALSE;
static struct InfoData      *info_data = NULL;
static char                  cmdbuf[BUFSIZE];

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
   log(LOG_DEBUG, "closedown procedure starts.\n");
   if (serial_open)
   {
      log(LOG_DEBUG, "closedown: AbortIO\n");
      AbortIO((struct IORequest*)io_serial);
      log(LOG_DEBUG, "closedown: WaitIO\n");
      WaitIO((struct IORequest*)io_serial);
      log(LOG_DEBUG, "closedown: CloseDevice\n");
      CloseDevice((struct IORequest*)io_serial);
   }
   if (io_serial)
   {
      log(LOG_DEBUG, "closedown: DeleteExtIO\n");
      DeleteExtIO( (struct IORequest *) io_serial);
   }
   if (mp_serial)
   {
      log(LOG_DEBUG, "closedown: DeletePort\n");
      DeletePort(mp_serial);
   }
   if (io_file) 
   {
      log(LOG_DEBUG, "closedown: Close file\n");
      Close((BPTR) io_file);
   }
   if (lock)
   {
      UnLock((BPTR) lock);
   }
   if (timer_open)
   {
      log(LOG_DEBUG, "closedown: AbortIO timer\n");
      AbortIO((struct IORequest*)&io_tr);
      log(LOG_DEBUG, "closedown: WaitIO timer\n");
      WaitIO((struct IORequest*)&io_tr);
      log(LOG_DEBUG, "closedown: CloseDevice timer\n");
      CloseDevice((struct IORequest*)&io_tr);
   }
   if (fib)
   {
      log(LOG_DEBUG, "closedown: free fib\n");
      FreeMem(fib, sizeof(struct FileInfoBlock));
   }
   if (dirbuf)
   {
      log(LOG_DEBUG, "closedown: free dirbuf\n");
      FreeMem(dirbuf, DIRBUF_SIZE);
   }
   if (info_data)
   {
      log(LOG_DEBUG, "closedown: free info_data\n");
      FreeMem(info_data, sizeof(struct InfoData));
   }
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
   if (DoIO( (struct IORequest*) io_serial))
   {
      log (LOG_ERROR, "*** ERROR: failed to set serial parameters!\n");
      closedown();
   }
}

static int read_serial(int len, UBYTE *buf)
{
   ULONG signals;
   int   todo   = len;
   int   offset = 0;
   BOOL  timeout = FALSE;

   while ( !timeout && (todo > 0) )
   {
      log(LOG_DEBUG2, "reading %d bytes at off %d from serial port...\n", 
          todo, offset);
      io_serial->IOSer.io_Command = CMD_READ;
      io_serial->IOSer.io_Length  = todo;
      io_serial->IOSer.io_Data    = (APTR) (buf + offset);
      SendIO( (struct IORequest*) io_serial);

      io_tr.tr_node.io_Command              = TR_ADDREQUEST;
      io_tr.tr_node.io_Message.mn_ReplyPort = mp_serial;
      io_tr.tr_time.tv_secs                 = SERIAL_TIMEOUT_SECS;
      io_tr.tr_time.tv_micro                = SERIAL_TIMEOUT_MIRCO;
      SendIO( (struct IORequest*) &io_tr);

      while ( !timeout )
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
#ifdef DEBUG_BYTES
            log(LOG_DEBUG2, "%ld bytes received:", len_actual);
            for (i=offset; (i<len_actual && i<9); i++)
               log(LOG_DEBUG2, " %02x", buf[i]);
            log(LOG_DEBUG2, "\n");
#endif

            todo   -= len_actual;
            offset += len_actual; 

            AbortIO((struct IORequest*)&io_tr);
            WaitIO((struct IORequest*)&io_tr);

            break;
         }

         if (CheckIO((struct IORequest*) &io_tr))
         {
            log (LOG_DEBUG2,"ERR : serial read timeout after %d bytes!\n", offset);
            WaitIO((struct IORequest*) &io_tr);

            AbortIO((struct IORequest*)io_serial);
            WaitIO((struct IORequest*)io_serial);

            timeout = TRUE;

            break;
         }
      }
   }

   return offset;
}

static void skip_serial_pending(void)
{

   UBYTE scratch[READSIZE];

   /* read until serial device is "empty" 
      (used for re-sync purposes)          */

   while (1)
   {
      int i, len_actual=0;
   
      len_actual = read_serial(READSIZE, scratch);

      if (len_actual <= 0)
         break;
   }

   log(LOG_DEBUG, "SYNC: skip_serial_pending done.\n");
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
   log (LOG_DEBUG, "ACK\n");
   write_serial(4, (UBYTE*) "PkOk");
}

static void write_nack(void)
{
   log (LOG_DEBUG, "NACK\n");
   write_serial(4, (UBYTE*) "PkRs");
}

static void read_message(struct ax_header *header, UBYTE *payload, int max_len)
{
   while (TRUE)
   {
      ULONG crc2;
      int len_actual;

      /* header */

      len_actual = read_serial(12, (UBYTE *) header);

      if (len_actual == 0)
         continue;

      crc2 = crc32((UBYTE *) header, 8);

      log (LOG_DEBUG, "MSG : cmd=0x%02x len=%d seq=%d crc=%08x crc2=%08x lena=%d\n",
           header->msg, header->len, header->seq, header->crc, crc2, len_actual);

      if ( (len_actual != 12) || (header->crc != crc2) )
      {
         log (LOG_ERROR, "ERR : corrupted message header\n");
         skip_serial_pending(); /* skip payload, if any */
         write_nack();
         continue;
      }
      /* FIXME: check sequence! */

      /* payload, if any */

      if (header->len)
      {
         ULONG crc1;
         if (header->len > max_len)
         {
            log (LOG_ERROR, "ERR : buffer overflow (%d > %d)\n", 
	         header->len, max_len);
	    closedown();
	 }
         len_actual = read_serial(header->len, payload);
         read_serial(4, (UBYTE *) &crc1);
         crc2 = crc32(payload, header->len);
         if ( (len_actual != header->len) || (crc1 != crc2) )
         {
            log (LOG_ERROR, "ERR : corrupted payload data (CRC: %08x vs %08x, len: %d vs %d)\n",
                 crc1, crc2, len_actual, header->len);
            write_nack();
            continue;
         }
      }
      break;
   }
   write_ack();
}

static ULONG read_ack(void)
{
   ULONG ack = 0xDEADBEEF;
   read_serial(4, (UBYTE*) &ack);
   return ack;
}

static void write_message(WORD msg, UBYTE *payload, int len)
{
   static ULONG seq = 0;
   struct ax_header header;

   header.sync = 0;
   header.msg  = msg;
   header.len  = len;
   header.seq  = seq++;
   header.crc  = crc32((UBYTE*)&header, 8);

   log (LOG_DEBUG, "WMSG: cmd=0x%02x len=%d seq=%d crc=%08x\n",
        header.msg, header.len, header.seq, header.crc);

   while (TRUE)
   {
      ULONG ack;

      write_serial(12, (UBYTE*) &header);

      /* payload, if any */
      if (len)
      {
         ULONG crc1;
         write_serial(len, payload);
         crc1 = crc32(payload, len);
         write_serial(4, (UBYTE*) &crc1);
      }

      ack = read_ack();
      if (ack != 0x506b4f6b /* PkOk */)
      {
         log (LOG_ERROR, "ERR : read_ack failed! (got: 0x%08x)\n", ack);
         if (ack == 0x506b5273) /* PkRs */
         {
            skip_serial_pending();
            continue;
         }
      }
      break;
   }
}

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
      write_message (MSG_IOERR, NULL, 0);
      UnLock ((BPTR)file_lock);
   } 
   else
   {    
      /* are we creating a directory here ? */

      if (recv.file_type == AX_FILE_TYPE_DIR)
      {
         if (lock)
         {
            UnLock((BPTR) lock);
            lock = NULL;
         }
         lock = (struct Lock *) CreateDir(filename);
         if (lock)
         {
            log(LOG_DEBUG, "    makedir(%s) succeeded.\n", filename);
            write_message (MSG_NEXT_PART, NULL, 0);
         }
         else
         {
            log(LOG_ERROR, "ERR makedir(%s) failed.\n", filename);
            write_message (MSG_IOERR, NULL, 0);
         }
      }
      else
      {
         write_message (MSG_NEXT_PART, NULL, 0);
      }
   }
}

static void msg_mparth (UBYTE *buf, WORD len)
{
   /* FIXME: implement? lxamiga.pl puts a constant 0x000002000 here */
   ULONG unk = *( ((ULONG*) buf) + 1 ); 

   receiving = *( (ULONG*) buf );
   received  = 0;
   sending   = 0;

   log(LOG_DEBUG, "msg_mparth receiving=%d, flags=%0x08x\n", receiving, io_flags);

   if (io_file)
      Close((BPTR)io_file);

   io_file = (struct FileHandle *) Open(filename, MODE_NEWFILE);
   if (!io_file)
   {
      log(LOG_ERROR, "*** ERROR: couldn´t open file for writing: %s\n",
          filename);
      write_message(MSG_IOERR, NULL, 0);
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
   receiving      = 0;
   sending        = 0;
   dirbuf_sending = FALSE;
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
      write_message(MSG_IOERR, NULL, 0);
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
      if (dirbuf_sending)
      {
         ULONG l = dirbuf_todo > BUFSIZE-4 ? BUFSIZE-4 : dirbuf_todo;

         log(LOG_DEBUG, "msg_next_part send dir %d\n", dirbuf_done);

         if (l>0)
         {
            *((ULONG*)buf) = dirbuf_done;
            CopyMem(dirbuf+dirbuf_done, (char*)buf+4, l);
	    write_message(MSG_BLOCK, buf, l+4);
            dirbuf_todo -= l;
            dirbuf_done += l;
         }
         else
         {
	    write_message(MSG_EOF, NULL, 0);
            dirbuf_todo=0;
            dirbuf_done=0;
            dirbuf_sending=FALSE;
         }
      }
      else
      {
         log(LOG_DEBUG, "SYNC\n");
      }
   }
}

static void BSTR2C(BSTR *bstr, char *buf)
{
   LONG   counter = 0;
   UBYTE *str = (UBYTE*) BADDR(bstr);
   LONG   i;
   
   for (i = (LONG) str[0]; i--; counter++)
   {
      buf[counter] = str[counter+1];
   }
   buf[counter]=0;
}

static void msg_dir (UBYTE *buf, WORD len)
{
   strncpy (filename, (char *)buf, PATH_MAX);
   filename[PATH_MAX-1] = 0;

   log(LOG_DEBUG, "msg_dir %s\n", filename);

   if (lock)
   {
      UnLock((BPTR) lock);
      lock = NULL;
   }

   if (strlen(filename)>0)
   {
      lock = (struct Lock *) Lock(filename, ACCESS_READ);
      if (lock)
      {
         if (Examine((BPTR)lock, (BPTR)fib))
         {

            log (LOG_DEBUG, "DIR %s size=%d, blocks=%d, dirtype=%d, type=%d\n", 
                 fib->fib_FileName,
                 fib->fib_Size,
                 fib->fib_NumBlocks,
                 fib->fib_DirEntryType,
                 fib->fib_EntryType);

            if (fib->fib_DirEntryType>0)
            {
               char  *dirbuf_ptr  = dirbuf + 4;
               ULONG  dirbuf_size = 0;
               ULONG  dir_cnt     = 0;

               dirbuf_todo = 4;

               while (ExNext((BPTR)lock, (BPTR)fib))
               {
                  ULONG entry_size, n, m;
                  struct ax_dirent *dirent;

                  n = strlen(fib->fib_FileName)+1;
                  m = strlen(fib->fib_Comment)+1;
                  entry_size = 29 + m + n;

                  log (LOG_DEBUG, "    %s size=%d, blocks=%d, dirtype=%d, type=%d, n=%d, m=%d, entry_size=%d\n", 
                       fib->fib_FileName,
                       fib->fib_Size,
                       fib->fib_NumBlocks,
                       fib->fib_DirEntryType,
                       fib->fib_EntryType,
                       n, m, entry_size);

                  if ( (dirbuf_todo + entry_size) > DIRBUF_SIZE )
                  {
                     log (LOG_ERROR, "ERR  *** dirbuf overflow!\n");
                     break;
                  }
                  dirent = (struct ax_dirent *) dirbuf_ptr;
                  dirent->len   = entry_size ;
                  dirent->size  = fib->fib_Size ;
                  dirent->used  = fib->fib_Size ;
                  dirent->type  = 0 ;
                  dirent->attrs = fib->fib_Protection ;
                  dirent->date  = fib->fib_Date.ds_Days ;
                  dirent->time  = fib->fib_Date.ds_Minute ;
                  dirent->ctime = fib->fib_Date.ds_Minute ;
                  dirent->type2 = fib->fib_DirEntryType > 0 ? 0x02 : 0x00 ;

                  dirbuf_ptr += 29;
                  CopyMem(fib->fib_FileName, dirbuf_ptr, n);
                  dirbuf_ptr += n;
                  CopyMem(fib->fib_Comment, dirbuf_ptr, m);
                  dirbuf_ptr += m;

                  dirbuf_todo = (LONG)dirbuf_ptr - (LONG)dirbuf;

                  dir_cnt += 1;
               }

               *((ULONG *) dirbuf) = dir_cnt;

               UnLock((BPTR) lock);
               lock = NULL;
               sending = 0;
               dirbuf_sending = TRUE;
               dirbuf_done = 0;
               write_message(MSG_MPARTH, (UBYTE*)&dirbuf_todo, 4); 
            }
            else
            {
               UnLock((BPTR) lock);
               lock = NULL;
               log (LOG_ERROR, "ERR  not a directory: %s\n", filename);
               write_message(MSG_EOF, NULL, 0); 
            }
         }
         else
         {
            UnLock((BPTR) lock);
            lock = NULL;
            log (LOG_ERROR, "ERR  examine() failed on %s\n", filename);
            write_message(MSG_EOF, NULL, 0);
         }
      }
      else
      {
         log (LOG_ERROR, "ERR  lock() failed on %s\n", filename);
         write_message(MSG_EOF, NULL, 0);
      }
   }
   else /* filename=="" -> list devices */
   {
      /* we have to extract these from internal dos.library
          structures in order to stay kickstart 1.3 compatible  */
  
      struct RootNode   *rootnode;
      struct DosInfo    *dosinfo;
      struct DeviceList *devicelist;
      struct FileLock   *filelock;
   
      char              *dirbuf_ptr  = dirbuf + 4;
      ULONG              dirbuf_size = 0;
      ULONG              dir_cnt     = 0;

      Forbid();
      
      rootnode   = (struct RootNode*)   DOSBase->dl_Root;
      dosinfo    = (struct DosInfo *)   BADDR(rootnode->rn_Info);
      devicelist = (struct DeviceList*) BADDR(dosinfo->di_DevInfo);

      dirbuf_todo = 4;

      while (devicelist->dl_Next)
      {
         char              devname[257];
         ULONG             entry_size, n, m;
         struct ax_dirent *dirent;
         BPTR              dev_lock = NULL;

         if (devicelist->dl_Type != DLT_VOLUME) 
         {
            devicelist = (struct DeviceList *) BADDR(devicelist->dl_Next);
            continue;
         }

         BSTR2C(devicelist->dl_Name, devname);
         devname[strlen(devname)+1] = 0; 
         devname[strlen(devname)]   = 58; /* append : */
         
         log (LOG_DEBUG, "    %s\n", devname);

         dev_lock = Lock(devname, ACCESS_READ);
         if (dev_lock)
         {
            n = strlen(devname)+1;
            m = 1;
            entry_size = 29 + m + n;

            if ( (dirbuf_todo + entry_size) > DIRBUF_SIZE )
            {
               log (LOG_ERROR, "ERR  *** dirbuf overflow!\n");
               UnLock(dev_lock);
               break;
            }

            if (Info(dev_lock, info_data))
            {
               dirent = (struct ax_dirent *) dirbuf_ptr;
               dirent->len   = entry_size ;
               
               dirent->size  = info_data->id_NumBlocks * info_data->id_BytesPerBlock ;
               dirent->used  = info_data->id_NumBlocksUsed * info_data->id_BytesPerBlock ;
               dirent->type  = 0x0000 ;
               dirent->attrs = info_data->id_DiskState == ID_WRITE_PROTECTED ? 0x04 : 0x00 ;
               dirent->date  = devicelist->dl_VolumeDate.ds_Days ;
               dirent->time  = devicelist->dl_VolumeDate.ds_Minute ;
               dirent->ctime = devicelist->dl_VolumeDate.ds_Minute ; 
               dirent->type2 = 0 ;
               
               dirbuf_ptr += 29;
               CopyMem(devname, dirbuf_ptr, n);
               dirbuf_ptr += n;
               *dirbuf_ptr=0;
               dirbuf_ptr += m;

               dirbuf_todo = (LONG)dirbuf_ptr - (LONG)dirbuf;

               dir_cnt += 1;
            }
            UnLock(dev_lock);
         }
         devicelist = (struct DeviceList *) BADDR(devicelist->dl_Next);
      }
      
      Permit();

      *((ULONG *) dirbuf) = dir_cnt;

      sending = 0;
      dirbuf_sending = TRUE;
      dirbuf_done = 0;
      write_message(MSG_MPARTH, (UBYTE*)&dirbuf_todo, 4); 
   }
}

static void msg_file_delete (UBYTE *buf, WORD len)
{
   int    l;
   BOOL   success = FALSE;

   strncpy (filename, (char *)buf, PATH_MAX);
   filename[PATH_MAX-1] = 0;

   log(LOG_DEBUG, "msg_file_delete %s\n", filename);

   l = strlen(filename);
   if ( (l>0) && (l<(BUFSIZE-30)) )
   {
      sprintf(cmdbuf, "delete \"%s\" ALL FORCE QUIET", filename); 
      log(LOG_DEBUG, "    execute %s\n", cmdbuf);
      success = Execute(cmdbuf,0,0);
   }

   if (success)
      write_message(MSG_NEXT_PART, NULL, 0);
   else
      write_message(MSG_IOERR, NULL, 0);
}

static void msg_file_rename (UBYTE *buf, WORD len)
{
   int              n;
   BOOL             success;
   struct FileLock *parent_lock, *old_lock;

   strncpy (filename, (char *)buf, PATH_MAX);
   filename[PATH_MAX-1] = 0;
   n = strlen(filename);

   strncpy (newname, (char *)(buf+n+1), PATH_MAX);
   newname[PATH_MAX-1] = 0;

   log(LOG_DEBUG, "msg_file_rename %s -> %s\n", filename, newname);

   if (lock)
   {
      UnLock((BPTR) lock);
   }

   lock = (struct Lock *) Lock(filename, ACCESS_READ);
  
   if (!lock)
   {
      log (LOG_ERROR, "ERR cannot lock %s\n", filename);
      write_message(MSG_IOERR, NULL, 0);
      return;
   }

   parent_lock = ParentDir((struct FileLock *) lock);

   UnLock((BPTR) lock);
   lock = NULL;

   if (!parent_lock)
   {
      log (LOG_ERROR, "ERR failed to find parent lock od %s\n", filename);
      write_message(MSG_IOERR, NULL, 0);
      return;
   }

   old_lock = (struct FileLock *) CurrentDir(parent_lock);

   success = Rename(filename, newname);

   CurrentDir(old_lock);
   UnLock((BPTR) parent_lock);

   if (success)
      write_message(MSG_NEXT_PART, NULL, 0);
   else
      write_message(MSG_IOERR, NULL, 0);
}

static void msg_file_move (UBYTE *buf, WORD len)
{
   int  n;
   BOOL success;

   strncpy (filename, (char *)buf, PATH_MAX);
   filename[PATH_MAX-1] = 0;
   n = strlen(filename);

   strncpy (newname, (char *)(buf+n+1), PATH_MAX);
   newname[PATH_MAX-1] = 0;

   log(LOG_DEBUG, "msg_file_move %s -> %s\n", filename, newname);

   /* this needs to work across devices.
      We´ll try a simple rename first. If that fails,
      we´ll user copy + remove                        */

   sprintf(cmdbuf, "rename >NIL: \"%s\" TO \"%s\"", filename, newname); 
   log(LOG_DEBUG, "    execute %s\n", cmdbuf);
   success = Execute(cmdbuf,0,0) && (IoErr()==0);

   if (success)
   {
      write_message(MSG_NEXT_PART, NULL, 0);
   }
   else
   {
      /* ok, copy+remove it is */
      sprintf(cmdbuf, "copy \"%s\" TO \"%s\"", filename, newname); 
      log(LOG_DEBUG, "    execute %s\n", cmdbuf);
      success = Execute(cmdbuf,0,0) && (IoErr()==0);
      if (!success)
      {
         log(LOG_ERROR, "ERR  failed %s\n", cmdbuf);
         write_message(MSG_IOERR, NULL, 0);
      }
      else
      {
         sprintf(cmdbuf, "delete \"%s\" QUIET", filename); 
         log(LOG_DEBUG, "    execute %s\n", cmdbuf);
         success = Execute(cmdbuf,0,0);
         if (success)
            write_message(MSG_NEXT_PART, NULL, 0);
         else
            write_message(MSG_IOERR, NULL, 0);
      }
   }
}

static void msg_file_copy (UBYTE *buf, WORD len)
{
   int  n;
   BOOL success;

   strncpy (filename, (char *)buf, PATH_MAX);
   filename[PATH_MAX-1] = 0;
   n = strlen(filename);

   strncpy (newname, (char *)(buf+n+1), PATH_MAX);
   newname[PATH_MAX-1] = 0;

   log(LOG_DEBUG, "msg_file_copy %s -> %s\n", filename, newname);

   sprintf(cmdbuf, "copy \"%s\" TO \"%s\"", filename, newname); 
   log(LOG_DEBUG, "    execute %s\n", cmdbuf);
   success = Execute(cmdbuf,0,0) && (IoErr()==0);
   if (success)
      write_message(MSG_NEXT_PART, NULL, 0);
   else
      write_message(MSG_IOERR, NULL, 0);
}

static void msg_file_attr (UBYTE *buf, WORD len)
{
   int  n;
   BOOL success = TRUE;
   LONG attrs;

   attrs = *((LONG *) buf);

   strncpy (filename, (char *)buf+4, PATH_MAX);
   filename[PATH_MAX-1] = 0;
   n = strlen(filename);

   strncpy (newname, (char *)(buf+n+5), PATH_MAX);
   newname[PATH_MAX-1] = 0;

   log(LOG_DEBUG, "msg_file_attrs %s (attr=0x%08x, comment=%s)\n", 
       filename, attrs, newname);

   success &= SetProtection(filename, attrs);
   success &= SetComment(filename, newname);

   if (success)
      write_message(MSG_NEXT_PART, NULL, 0);
   else
      write_message(MSG_IOERR, NULL, 0);
}

static void msg_close (UBYTE *buf, WORD len)
{
   if (io_file)
   {
      Close((BPTR) io_file);
      SetProtection(filename, recv.attrs);
      if (DOSBase->dl_lib.lib_Version >= 36)
      {
         struct DateStamp ds;
         ds.ds_Days   = recv.date;
         ds.ds_Minute = recv.time;
         ds.ds_Tick   = 0;
         SetFileDate(filename, &ds);
      }
      io_file = NULL;
   }
   if (lock)
   {
      UnLock ((BPTR)lock);
      lock = NULL;
   } 
   write_message(MSG_ACK_CLOSE, NULL, 0);
}

int main(int argc, char **argv)
{
   UBYTE buf_serial[BUFSIZE];
   ULONG signals;
   struct ax_header header;

   log (LOG_INFO, "FTS4 %s (C) 2019 by G. Bartsch\n\n", VERSION);

   parse_commandline(argc, argv);

   log (LOG_INFO, "detected dos.library version %d rev %d\n",
        DOSBase->dl_lib.lib_Version, DOSBase->dl_lib.lib_Revision);

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

   io_serial->io_RBufLen  = BUFSIZE;
   io_serial->io_ExtFlags = 0;
   io_serial->io_ReadLen  = 8 ;
   io_serial->io_WriteLen = 8 ;
   io_serial->io_StopBits = 1 ;
   io_serial->io_SerFlags = SERF_XDISABLED | SERF_7WIRE ;
   serial_open = !OpenDevice(device_name, 0, (struct IORequest*)io_serial, 0);
   if (!serial_open)
   {
      log (LOG_ERROR, "ERROR: %s did not open.\n", device_name);
      closedown();
   }

   wait_mask = SIGBREAKF_CTRL_C | 1L << mp_serial->mp_SigBit;

   setup_serial(baudrate);

   timer_open = !OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest*) &io_tr, 0);
   if (!timer_open)
   {
      log (LOG_ERROR, "ERROR: timer.device did not open.\n");
      closedown();
   }

   fib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock), 0);
   if (!fib)
   {
      log (LOG_ERROR, "ERROR: out of memory (fib).\n");
      closedown();
   }

   dirbuf = AllocMem(DIRBUF_SIZE, 0);
   if (!dirbuf)
   {
      log (LOG_ERROR, "ERROR: out of memory (dirbuf).\n");
      closedown();
   }

   info_data = AllocMem(sizeof(struct InfoData), 0);
   if (!info_data)
   {
      log (LOG_ERROR, "ERROR: out of memory (info_data).\n");
      closedown();
   }

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
            msg_close(buf_serial, header.len);
            break;

         case MSG_FILE_SEND:
            msg_file_send(buf_serial, header.len);
            break;

         case MSG_FILE_DELETE:
            msg_file_delete(buf_serial, header.len);
            break;

         case MSG_FILE_RENAME:
            msg_file_rename(buf_serial, header.len);
            break;

         case MSG_FILE_MOVE:
            msg_file_move(buf_serial, header.len);
            break;

         case MSG_FILE_COPY:
            msg_file_copy(buf_serial, header.len);
            break;

         case MSG_FILE_ATTR:
            msg_file_attr(buf_serial, header.len);
            break;

         case MSG_NEXT_PART:
            msg_next_part(buf_serial, header.len);
            break;

         case MSG_DIR:
            msg_dir(buf_serial, header.len);
            break;

         default:
            log (LOG_ERROR, "*** ERROR: unknown message 0x%04x received!\n",
                 header.msg);
            closedown();
      }
   }
}

