/* Minimal freestanding support for compiler-emitted struct copies/zeroes.
 * Volatile bytes avoid unaligned/vector accesses with EL1 MMU disabled. */
#include <stddef.h>
void *memcpy(void *destination,const void *source,size_t size)
{
 volatile unsigned char *d=destination;
 const volatile unsigned char *s=source;
 while(size--)*d++=*s++;
 return destination;
}
void *memset(void *destination,int value,size_t size)
{
 volatile unsigned char *d=destination;
 while(size--)*d++=(unsigned char)value;
 return destination;
}
