#define CASE_KIND 0

void transaction_edges(unsigned output[16], unsigned seed, unsigned count)
{
#if CASE_KIND == 3
    if (seed & 1) return;
#endif
    unsigned x = CASE_KIND == 1 ? seed : 7;
    unsigned trips = CASE_KIND == 2 ? count & 15 : 16;
    for (unsigned i = 0; i < trips; ++i) {
#if CASE_KIND == 4
        x = (unsigned)((int)x >> 3) ^ (x << 5) ^ 0x9e3779b9U;
#elif CASE_KIND == 5
        x = (x << (seed & 7)) ^ (x >> ((seed >> 4) & 7)) ^ 0x9e3779b9U;
#else
        x = (x << 1) ^ (x >> 1) ^ 0x9e3779b9U;
#endif
        output[i] = x ^ seed;
    }
}
