/* Two-cell initializer straddles 64000, and zero globals span more than a bank. */
unsigned char padding[63998];
unsigned long crossing = 0x12345678UL;
unsigned long *crossing_pointer = &crossing;
unsigned char trailing[64003];
int main(void)
{
    if (padding[0] || padding[63997] || trailing[0] || trailing[64002]) return 1;
    if (crossing != 0x12345678UL || *crossing_pointer != crossing) return 2;
    padding[63997] = 511U;
    trailing[64002] = 777U;
    *crossing_pointer = 0x98765432UL;
    if (padding[63997] != 511U || trailing[64002] != 777U || crossing != 0x98765432UL) return 3;
    return 0;
}
