#include "libraries/dosextens.h"
#include "stdio.h"

#define BUFSIZE 255

extern struct FileHandle *Open();

int main(void)
{
   struct FileHandle *fh_ser, *fh_trans;
   char buf[BUFSIZE];
   char filename[BUFSIZE];
   int n, sent, total;
   unsigned char c;

   printf("FTS v4.1 (C) 2019 by G. Bartsch\n\n");

   fh_trans = NULL;

   printf("opening serial port\n");

   fh_ser = Open("SER:", MODE_OLDFILE);
   if (fh_ser == NULL)
   {
      printf("ERROR: failed to open serial port!\n");
      exit(1);
   }

   /* wait for handshake */
   buf[0]=0;
   n=0;
   while ( (buf[0]!=42) || (n!=1) )
   {
      printf("waiting for handshake...\n");
      n = Read(fh_ser, buf, 1);
      printf("got %d bytes, buf[0]: %d\n", n, buf[0]);
      if (n==-1)
      {
         printf("ERROR: failed to read handshake byte!\n");
         goto closedown;
      }
   }
 
   /* handshake reply */
   buf[0] = 23;
   Write(fh_ser, buf, 1);
 
   /* read command byte and filename len */
   n = Read(fh_ser, buf, 2);
   if (n != 2)
   {
      printf("ERROR: failed to read command bytes!\n");
      goto closedown;
   }

   /* read filenme */
   n = Read(fh_ser, filename, buf[1]);
   if (n != buf[1])
   {
      printf("ERROR: failed to read filename!\n");
      goto closedown;
   }
   printf("filename: %s\n", filename);

   if (buf[0] == 23)
   {
      /* send file */
      fh_trans = Open(filename, MODE_OLDFILE);
      if (fh_trans == NULL)
      {
         printf("ERROR: failed to open file!\n");
         goto closedown;
      }

      /* determine file size */
      total = Seek(fh_trans, 0, OFFSET_END);
      total = Seek(fh_trans, 0, OFFSET_BEGINNING);
      sent = 0;
      while (1)
      {
         n = Read(fh_trans, buf, BUFSIZE);
         if (n==0)
         {
            printf("finished!\n");
            c = (unsigned char) 0;
            Write(fh_ser, &c, 1);
            break;
         } 
         else if (n<0)
         {
            printf("ERROR reading file!\n");
            c = (unsigned char) 0;
            Write(fh_ser, &c, 1);
            break;
         } 
         else
         {
	    sent += n;
            printf("sending %3d [%7d/%7d] bytes...\n", n, sent, total);
            c = (unsigned char) n;
            Write(fh_ser, &c, 1);
            Write(fh_ser, buf, n);
            Read(fh_ser, &c, 1);
            if (c != 42)
            {
               printf("ERROR: sync lost!\n");
               break;
            }
         } 
      }
   }
   else if (buf[0] == 24)
   {
      /* receive file */
      fh_trans = Open(filename, MODE_NEWFILE);
      if (fh_trans == NULL)
      {
         printf("ERROR: failed to open file!\n");
         goto closedown;
      }
      while (1)
      {
         n = Read(fh_ser, &c, 1);
	 if (n==0)
            continue;
         if (c==0)
         {
            printf("finished!\n");
            goto closedown;
         }
         printf("receiving %d bytes...\n", c);
         n = Read(fh_ser, buf, c);
	 if (n != c)
         {
            printf("ERR got %d bytes, expected %d\n", n, c);
            goto closedown;
         }
	 Write(fh_trans, buf, c);
	 /* send ack */
         c = 23;
	 Write(fh_ser, &c, 1);
      }
   }
   else
   {
      printf ("ERROR: command %d is not implemented (yet?).\n", buf[0]);
   }

closedown:

   if (fh_trans != NULL)
   {
      printf("closing transfered file.\n");
      Close(fh_trans);
   }

   printf("closing serial port\n");
   Close(fh_ser);

   printf("goodbye.\n\n");
}

