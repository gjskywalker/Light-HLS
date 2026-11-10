#define CASE_KIND 0

void outer_edges(unsigned input[16], unsigned output[16], unsigned seed, unsigned count)
{
    unsigned limit = CASE_KIND == 1 ? count & 15 : (CASE_KIND == 8 ? 1 : 16);
    unsigned step = CASE_KIND == 3 ? (seed & 3) + 1 : 1;
#if CASE_KIND == 2
    for (unsigned i = 16; i != 0; --i) {
        unsigned index = i - 1;
#else
    for (unsigned i = 0; i < limit; i += step) {
        unsigned index = i;
#endif
        unsigned initial = input[index];
        unsigned x = initial;
#if CASE_KIND == 6
        // The retained load also feeds a multicycle live-out expression.
        unsigned slow = initial / (seed | 1);
#endif
#if CASE_KIND == 8
        // The second response replaces Q before the first value reaches its PHI.
        unsigned y = input[1];
#endif
#if CASE_KIND == 5 || CASE_KIND == 9
        if ((CASE_KIND == 5 ? count : initial) & 1) {
            for (unsigned j = 0; j < 4; ++j)
                x ^= x << 1;
        } else
#endif
        {
            for (unsigned j = 0; j < 8; ++j) {
                // PIPELINE_DIRECTIVE
#if CASE_KIND == 8
                unsigned old_x = x;
                x = (x << 1) ^ y;
                y = (y >> 1) ^ old_x;
#else
                x = (x << 1) ^ (x >> 1) ^ 0x9e3779b9U;
#endif
            }
        }
#if CASE_KIND == 4
        // The initial load remains live after the inner loop.
        output[index] = x ^ initial;
#elif CASE_KIND == 6
        output[index] = x ^ slow;
#elif CASE_KIND == 8
        output[index] = x ^ y;
#else
        output[index] = x ^ seed;
#endif
    }
}
