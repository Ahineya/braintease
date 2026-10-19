struct Pair {int tag; long value;};
struct Record {struct Pair pair; double weight; int *pointer; int data[3];};
union Word {long wide; unsigned int cells[2];};
struct Pair shared={2,70000L};
unsigned int storage[64020];
volatile struct Pair observed={3,11L};
struct Marked {volatile int tag; long value;};
struct Marked marked={4,12L};

struct Pair make(int tag,long value) {
    struct Pair result={tag,value};
    return result;
}
struct Pair add(struct Pair a,struct Pair b) {
    a.tag+=b.tag;
    a.value+=b.value;
    return a;
}
struct Pair recurse(struct Pair p,int count) {
    if(count) return recurse(add(p,make(1,1L)),count-1);
    else return p;
}
struct Pair get_shared(void) {return shared;}
struct Pair change_shared(void) {shared.value=1L;return shared;}
int modify(struct Pair p,int marker,long wide,int *out) {
    p.tag=99;p.value=33L;*out=marker;
    return wide==0L && marker==7;
}
struct Record record(struct Record r,struct Pair p) {
    r.pair=add(r.pair,p);r.weight+=0.5;r.data[2]++;
    return r;
}
union Word rotate(union Word w) {w.wide+=1L;return w;}
struct Pair get_observed(void) {return observed;}
struct Marked get_marked(void) {return marked;}

int main(void) {
    int marker=0,local=9;
    struct Pair a=make(2,0L),b=make(3,70000L),c;
    struct Pair (*callback)(struct Pair,struct Pair)=add;
    struct Record r={.pair={4,8L},.weight=2.25,.pointer=&local,.data={1,2,3}};
    union Word w={.wide=0x12345678L};
    struct Pair *cross=(struct Pair *)(storage+(63999UL-((unsigned long)storage&65535UL)));
    if(!modify(b,7,0L,&marker) || marker!=7 || b.tag!=3 || b.value!=70000L)return 1;
    c=callback(a,add(make(4,2L),b));
    if(c.tag!=9 || c.value!=70002L)return 2;
    c=add(get_shared(),change_shared());
    if(c.value!=70001L)return 3;
    c=recurse(make(1,0L),5);
    if(c.tag!=6 || c.value!=5L)return 4;
    if(make(8,400000L).value!=400000L)return 5;
    c=(local==9?a:b);
    if(c.tag!=2 || c.value!=0L)return 6;
    if((c=b).value!=70000L)return 7;
    r=record(r,make(1,2L));
    if(r.pair.tag!=5 || r.pair.value!=10L || r.weight!=2.75 || *r.pointer!=9 || r.data[2]!=4)return 8;
    w=rotate(w);
    if(w.wide!=0x12345679L)return 9;
    cross->tag=17;cross->value=123456L;
    c=add(*cross,a);
    if(c.tag!=19 || c.value!=123456L || cross->tag!=17)return 10;
    c=get_observed();
    if(c.tag!=3 || c.value!=11L || get_marked().value!=12L)return 11;
    return 0;
}
