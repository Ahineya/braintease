struct Pair {int tag; long value;};
struct Entry {char *name; int *number; long *field; int (*callback)(int); double weight;};
int number=19;
int *number_ptr=&number;
int **number_ptr_ptr=&number_ptr;
int data[64010];
int *last=data+64009;
int *before=&data[7]-2;
int *one_past=&data[64010];
struct Pair pair={3,70000L};
long *field=&pair.value;
char *greeting="hello";
char *tail="hello"+2;
char *names[4]={[2]="two",[0]="zero"};
int twice(int n) {return n*2;}
int (*callback)(int)=twice;
int (*callbacks[3])(int)={twice,0,twice};
struct Entry entries[2]={
    {.name="first",.number=&number,.field=&pair.value,.callback=twice,.weight=2.25},
    {.name="second",.number=&data[7],.field=&pair.value,.callback=twice,.weight=1.5}
};
struct Link {int value; struct Link *next;};
struct Link links[2]={{4,&links[1]},{5,&links[0]}};
union Ref {int *pointer; long bits;};
union Ref ref={.pointer=&number};
int *null_pointer=0;
void *encoded=(void*)0x12340056UL;
void *normalized=(char*)0x0001f9ffUL+2;
void *integer_sum=(void*)(65535UL+1UL);
void *narrowed=(void*)(unsigned short)0x12345UL;
void *normalized_cast=(void*)(unsigned long)((char*)0x0001f9ffUL+2);
int *selected=1?&number:&data[0];
int *float_selected=-0.0?&data[0]:&number;
long raw_address=(long)&number;
int local_static(void) {static int *p=&number;return *p;}
int main(void) {
    int negative=-1;
    *last=31;*before=7;
    if(**number_ptr_ptr!=19 || data[64009]!=31 || data[5]!=7)return 1;
    if(one_past-data!=64010L || last-data!=64009L)return 2;
    if(*field!=70000L || tail[0]!='l' || greeting[1]!='e')return 3;
    if(names[0][0]!='z' || names[1]!=0 || names[2][0]!='t' || names[3]!=0)return 4;
    if(callback(3)!=6 || callbacks[1]!=0 || callbacks[2](4)!=8)return 5;
    if(entries[1].name[0]!='s' || *entries[0].number!=19 || *entries[1].field!=70000L)return 6;
    if(entries[0].callback(9)!=18 || entries[0].weight!=2.25 || entries[1].weight!=1.5)return 7;
    if(links[0].next->value!=5 || links[1].next!=&links[0])return 8;
    if(ref.pointer!=&number || null_pointer!=0 || selected!=&number || local_static()!=19)return 9;
    if((unsigned long)encoded!=0x12340056UL || (unsigned long)normalized!=0x00020001UL)return 10;
    if(raw_address!=(long)&number)return 11;
    if((unsigned long)integer_sum!=65536UL || (unsigned long)narrowed!=0x2345UL || normalized_cast!=normalized)return 12;
    if(!(void*)0x00010000UL || (void*)0x00010001UL==(void*)0x00020001UL || float_selected!=&number)return 13;
    if((void*)negative!=(void*)-1)return 14;
    return 0;
}
