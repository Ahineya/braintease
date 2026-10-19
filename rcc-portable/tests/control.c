int visits;
int visit(void) {visits++; return visits;}
int fib(int n) {if(n<2)return n; return fib(n-1)+fib(n-2);}
int choose(int n) {if(n)return 3;else return 4;}
int main(void) {
    int total=0;
    for(int i=0;i<12;i++) {if(i==4)continue;if(i==9)break;total+=i;}
    if(total!=32 || fib(8)!=21)return 1;
    if(choose(0)!=4 || choose(1)!=3)return 9;
    if(0 && visit())return 2;
    if(!(1 || visit()) || visits!=0)return 3;
    total=visit()?visit():visit();
    if(total!=2 || visits!=2)return 4;
    switch(total) {case 1:return 5;case 2:total=17;break;default:return 6;}
    while(total>12)total--;
    do {total--;} while(total>10);
    if(total!=10)return 7;
    goto done;
    total=0;
 done:
    return total==10?0:8;
}
