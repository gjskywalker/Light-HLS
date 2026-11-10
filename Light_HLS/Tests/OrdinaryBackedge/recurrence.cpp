#define CASE_KIND 0

void recurrence(unsigned input[16], unsigned output[16], unsigned seed, unsigned count)
{
    for (unsigned i = 0; i < 16; ++i) {
        unsigned x = input[i] ^ seed;
        unsigned y = seed + i;
        unsigned previous = x;
        unsigned trips = CASE_KIND == 1 ? (count & 7) + 1 : 8;
        for (unsigned j = 0; j < trips; ++j) {
            // PIPELINE_DIRECTIVE
#if CASE_KIND == 2
            unsigned next_x = (x << 1) ^ y;
            y = (y >> 1) + x;
            x = next_x;
#elif CASE_KIND == 3
            x = (x << 1) ^ input[j];
#elif CASE_KIND == 4
            x = (x << 1) ^ (x >> 1) ^ 0x9e3779b9U;
            // The read delays the backedge past the x issue edge.
            y += input[j];
#elif CASE_KIND == 6
            x = ((x << 1) ^ (x >> 1) ^ 0x9e3779b9U) & 255;
#elif CASE_KIND == 7
            x /= seed | 1;
#elif CASE_KIND == 8
            previous = x;
            x = (x << 1) ^ (x >> 1) ^ 0x9e3779b9U;
#else
            x = (x << 1) ^ (x >> 1) ^ 0x9e3779b9U;
#endif
        }
        output[i] = x ^ (CASE_KIND == 8 ? previous : y);
    }
}
