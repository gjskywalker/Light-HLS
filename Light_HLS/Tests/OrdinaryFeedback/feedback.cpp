#define CASE_KIND 0

unsigned feedback(unsigned input[256], unsigned output[16], unsigned seed,
                  unsigned count, unsigned step)
{
    unsigned char address = (unsigned char)seed;
    unsigned i = 0;
#if CASE_KIND == 2
    count &= 15;
#elif CASE_KIND == 4
    count = (count & 15) + 1;
#endif
#if CASE_KIND == 4
    // A data-dependent exit prevents replacing the final PHI with a trip formula.
    do {
        output[i & 15] = input[address] ^ ((unsigned)address << 24) ^ seed;
        address = (unsigned char)(address + 3);
        ++i;
    } while (i < count && address != (unsigned char)step);
#else
    for (i = 0; i < (CASE_KIND == 2 ? count : 16); ++i) {
#if CASE_KIND == 5
#pragma HLS PIPELINE II=1
#endif
        unsigned value = input[address];
        output[i & 15] = value ^ ((unsigned)address << 24) ^ seed;
#if CASE_KIND == 1
        address = (unsigned char)(address - 3);
#elif CASE_KIND == 3
        address = (unsigned char)(address + step);
#elif CASE_KIND == 6
        address = (unsigned char)(address + ((value & 1) ? 3 : -5));
#else
        address = (unsigned char)(address + 3);
#endif
    }
#endif
#if CASE_KIND == 7
    return (int)(signed char)address;
#else
    return address;
#endif
}
