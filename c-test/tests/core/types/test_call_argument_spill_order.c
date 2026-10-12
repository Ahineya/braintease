#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct object { int32_t x,y,z; int type; void *info; char *name; };
static int type_info;
static char object_name[]="spawn";
static void *allocate_object(uint32_t length,int tag,void **owner) {
    void *memory=malloc(length);
    type_info=tag;
    if(owner) *owner=memory;
    return memory;
}
static struct object *spawn(int32_t x,int32_t y,int32_t z,int type) {
    struct object *object;
    void *info;
    char *name;
    object=allocate_object(67UL,5,NULL);
    if(!object) return NULL;
    memset(object,0,67UL);
    info=&type_info;
    name=object_name;
    object->x=x;
    object->y=y;
    object->z=z;
    object->type=type;
    object->info=info;
    object->name=name;
    return object;
}
int main(void) {
    struct object *object=spawn(69206016L,-236978176L,0x12345678L,7);
    if(!object) return 1;
    putchar(object->x==69206016L?'Y':'N');
    putchar(object->y==-236978176L?'Y':'N');
    putchar(object->z==0x12345678L?'Y':'N');
    putchar(object->type==7 && object->info==&type_info && object->name==object_name?'Y':'N');
    putchar('\n'); free(object); return 0;
}
