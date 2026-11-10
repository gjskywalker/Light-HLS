#include <hls_stream.h>

static void produce_tail(unsigned input[16], unsigned audit[64],
                         hls::stream<unsigned> &tokens, unsigned seed)
{
    for (unsigned i = 0; i < 16; ++i)
        tokens.write(input[i] + seed);
    for (unsigned i = 0; i < 64; ++i)
        audit[i] = seed ^ (i * 17u);
}

static void consume_tail(hls::stream<unsigned> &tokens, unsigned output[16])
{
    for (unsigned i = 0; i < 16; ++i)
        output[i] = tokens.read() ^ 0x125678abu;
}

void late_effects(unsigned input[16], unsigned output[16], unsigned audit[64], unsigned seed)
{
#pragma HLS DATAFLOW
    hls::stream<unsigned> tokens;
#pragma HLS STREAM variable=tokens depth=2
    produce_tail(input, audit, tokens, seed);
    consume_tail(tokens, output);
}
