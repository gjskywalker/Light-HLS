#include <hls_stream.h>

static void produce(const unsigned input[16], hls::stream<unsigned> &tokens,
                    unsigned seed)
{
    for (unsigned i = 0; i < 16; ++i) {
        // PIPELINE_DIRECTIVE
        tokens.write(input[i] * seed + 17);
        tokens.write(0x12345678U);
        tokens.write(0x12345678U);
        tokens << (seed ^ i);
    }
}

static void consume(hls::stream<unsigned> &tokens, unsigned output[16])
{
    for (unsigned i = 0; i < 16; ++i) {
        // PIPELINE_DIRECTIVE
        unsigned first = tokens.read();
        tokens.read();
        unsigned third, last;
        tokens.read(third);
        tokens >> last;
        output[i] = (first + third) ^ last;
    }
}

void fifo_events(unsigned input[16], unsigned output[16], unsigned seed)
{
#pragma HLS DATAFLOW
    hls::stream<unsigned> tokens;
#pragma HLS STREAM variable=tokens depth=1
    produce(input, tokens, seed);
    consume(tokens, output);
}
