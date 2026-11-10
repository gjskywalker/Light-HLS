#define CASE_KIND 0

void scalar_bank_events(unsigned data[4], unsigned output[4], unsigned index, unsigned seed)
{
#pragma HLS ARRAY_PARTITION variable=data type=complete dim=1
#pragma HLS ARRAY_PARTITION variable=output type=complete dim=1
    for (unsigned i = 0; i < 4; ++i) {
#pragma HLS PIPELINE II=1
        unsigned a = (i + index) & 3;
        unsigned b = (a + 1) & 3;
        unsigned old_a = data[a];
        unsigned old_b = data[b];
#if CASE_KIND == 2
        output[i & 3] = old_a + old_b + seed;
#else
        data[b] = old_a + seed + i;
        output[i & 3] = old_b ^ data[b];
#endif
    }
}
