void putchar(int);
typedef unsigned char triple[3];
struct Entry { int x; long y; };
static struct Entry entries[] = { { 1, 123456L }, { 2, 789012L } };
static int slots[sizeof entries / sizeof(struct Entry)];
static unsigned long offset = (unsigned long)&((struct Entry *)0)->y;
typedef int (callback)(int);
void fill(triple data) { data[0]=7; data[1]=8; data[2]=9; }
unsigned size(triple data) { return sizeof data; }
int main(void) {
    triple data;
    int (values)[3] = { 10, 20, 30 };
    fill(data);
    putchar(data[0]==7 && data[2]==9 ? 'Y':'N');
    putchar(size(data)==2 && sizeof data==3 ? 'Y':'N');
    putchar(sizeof slots==2 && values[1]==20 ? 'Y':'N');
    {
        long entries[7];
        int local[sizeof entries / sizeof entries[0]];
        putchar(sizeof local==7 ? 'Y':'N');
    }
    putchar(sizeof entries==6 && offset==1 ? 'Y':'N');
    putchar('\n');
    return 0;
}
