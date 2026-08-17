// Compound literals with unions
void putchar(int c);

typedef union {
    int i;
    float f;
    char c[4];
} MultiType;

typedef struct {
    int type;
    union {
        int ival;
        char cval;
    } data;
} Tagged;

int main() {
    // Test 1: Simple union compound literal
    MultiType *m1 = &(MultiType){.i = 0x4344};
    if (m1->i == 0x4344) {
        putchar('Y');
    } else {
        putchar('N');
    }
    
    // Test 2: Union compound literal with char array
    MultiType m2 = (MultiType){.c = {'A', 'B', 'C', 'D'}};
    if (m2.c[0] == 'A' && m2.c[1] == 'B' && 
        m2.c[2] == 'C' && m2.c[3] == 'D') {
        putchar('Y');
    } else {
        putchar('N');
    }
    
    // Test 3: Tagged union with compound literal
    Tagged t1 = (Tagged){.type = 1, .data.ival = 100};
    if (t1.type == 1 && t1.data.ival == 100) {
        putchar('Y');
    } else {
        putchar('N');
    }
    
    // Test 4: Anonymous union in compound literal
    Tagged t2 = (Tagged){2, {.cval = 'X'}};
    if (t2.type == 2 && t2.data.cval == 'X') {
        putchar('Y');
    } else {
        putchar('N');
    }
    
    putchar('\n');
    return 0;
}