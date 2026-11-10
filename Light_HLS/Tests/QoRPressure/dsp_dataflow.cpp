#include <hls_stream.h>

static void load_pressure(unsigned input[256], hls::stream<unsigned> &tokens)
{
#pragma HLS INLINE off
    for (unsigned i = 0; i < 256; ++i)
    {
#pragma HLS PIPELINE II=1
        tokens.write(input[i]);
    }
}

static void compute_pressure(hls::stream<unsigned> &tokens,
                             hls::stream<unsigned> &results, unsigned coefficient)
{
#pragma HLS INLINE off
    for (unsigned i = 0; i < 256; ++i)
    {
#pragma HLS PIPELINE II=1
        results.write((tokens.read() & 0x1ffffu) * (coefficient & 0x1ffffu) + i);
    }
}

static void store_pressure(hls::stream<unsigned> &results, unsigned output[256])
{
#pragma HLS INLINE off
    for (unsigned i = 0; i < 256; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = results.read();
    }
}

void qor_pressure(unsigned input[256], unsigned output[256], unsigned coefficient)
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
#pragma HLS DATAFLOW
    hls::stream<unsigned> tokens;
    hls::stream<unsigned> results;
#pragma HLS STREAM variable=tokens depth=4
#pragma HLS STREAM variable=results depth=4
    load_pressure(input, tokens);
    compute_pressure(tokens, results, coefficient);
    store_pressure(results, output);
}
