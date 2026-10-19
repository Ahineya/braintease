#include <stdio.h>
#include <stdarg.h>
enum Tag { TAG_OBJECT=-1, TAG_INT=0, TAG_NULL=2 };
static int compare(unsigned long tag) { return tag == TAG_OBJECT; }
static int arguments(int count, ...) {
 va_list ap; int first, second;
 va_start(ap,count); first=va_arg(ap,int); second=va_arg(ap,int); va_end(ap);
 return first == -1 && second == 123;
}
int main(void) {
 unsigned long tag = (unsigned long)-1L;
 if (tag == TAG_OBJECT) putchar('Y'); else putchar('N');
 if (!(tag != TAG_OBJECT)) putchar('Y'); else putchar('N');
 if (compare(tag)) putchar('Y'); else putchar('N');
 if (((unsigned long)(enum Tag)TAG_OBJECT) == 0xffffffffUL) putchar('Y'); else putchar('N');
 if (sizeof(TAG_OBJECT) == sizeof(int)) putchar('Y'); else putchar('N');
 if (arguments(2,TAG_OBJECT,123)) putchar('Y'); else putchar('N');
 switch (tag) { case TAG_OBJECT: putchar('Y'); break; default: putchar('N'); }
 putchar('\n'); return 0;
}
