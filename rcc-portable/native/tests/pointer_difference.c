struct entry { unsigned long a, b; };
static long choose(struct entry *a, struct entry *b)
{
    return a - b < 3 ? a - b : 99;
}
int main(void)
{
    struct entry data[8];
    if (choose(data + 2, data) != 2) return 1;
    if (choose(data, data + 2) != -2) return 2;
    if (choose(data + 7, data) != 99) return 3;
    if ((data + 5 - data > 2) && choose(data + 1, data) != 1) return 4;
    return 0;
}
