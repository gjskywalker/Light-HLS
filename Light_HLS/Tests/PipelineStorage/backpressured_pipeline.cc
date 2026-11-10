#include <hls_stream.h>

static void fast_load(const int input[64], hls::stream<int> &loaded)
{
    for (int index = 0; index < 64; ++index)
    {
#pragma HLS PIPELINE II=1
        loaded.write(input[index]);
    }
}

static void fast_compute(hls::stream<int> &loaded, hls::stream<int> &computed)
{
    for (int index = 0; index < 64; ++index)
    {
#pragma HLS PIPELINE II=1
        computed.write(loaded.read() * 3 + 1);
    }
}

static void slow_store(hls::stream<int> &computed, int output[64])
{
    for (int index = 0; index < 64; ++index)
        output[index] = computed.read();
}

void backpressured_pipeline(const int input[64], int output[64])
{
    hls::stream<int> loaded;
    hls::stream<int> computed;
#pragma HLS STREAM variable=loaded depth=1 type=fifo
#pragma HLS STREAM variable=computed depth=1 type=fifo
#pragma HLS DATAFLOW
    fast_load(input, loaded);
    fast_compute(loaded, computed);
    slow_store(computed, output);
}
