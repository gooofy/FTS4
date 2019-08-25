#include "crc.h"

/* #define DEBUG */

static void _crc32 (unsigned char b, ULONG *reg32)
{
    int i;
    ULONG p = 0xedb88320;		/* generator polynom */

    for (i=0; i<8; ++i)
    {
        if ( (*reg32 & 1) != (b & 1) )
             *reg32 = (*reg32>>1)^p; 
        else 
             *reg32 >>= 1;
        b >>= 1;
    }
}

ULONG crc32(unsigned char *data, int len)
{
    int i;
    ULONG reg32 = 0xffffffff; /* shift register */

    for (i=0; i<len; i++)  {
        unsigned char x = data[i];
        _crc32(x, &reg32);
#ifdef DEBUG
        if (i>349 && i<399)
           printf ("crc32: i=%d, x=%02x, reg32=%08x\n", i, x, reg32);
#endif
    }

    return reg32 ^ 0xffffffff;
}

