#include <hls_stream.h>

static const int N = 64;

static void load_stage(const int input[N], hls::stream<int> &values)
{
    for (int i = 0; i < N; ++i)
    {
        values.write(input[i]);
    }
}

static void compute_stage(hls::stream<int> &input,
                          hls::stream<int> &output)
{
    for (int i = 0; i < N; ++i)
    {
        output.write(input.read() * 3 + 1);
    }
}

static void store_stage(hls::stream<int> &values, int output[N])
{
    for (int i = 0; i < N; ++i)
    {
        output[i] = values.read();
    }
}

void vector_stream_dataflow_depth1(const int input[N], int output[N])
{
    hls::stream<int> loaded;
    hls::stream<int> computed;

#pragma HLS STREAM variable=loaded depth=1 type=fifo
#pragma HLS STREAM variable=computed depth=1 type=fifo
#pragma HLS DATAFLOW
    load_stage(input, loaded);
    compute_stage(loaded, computed);
    store_stage(computed, output);
}
