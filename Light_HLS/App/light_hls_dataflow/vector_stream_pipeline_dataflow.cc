#include <hls_stream.h>

static const int N = 64;

static void load_stage(const int input[N], hls::stream<int> &values)
{
    for (int index = 0; index < N; ++index)
    {
#pragma HLS PIPELINE II=1
        values.write(input[index]);
    }
}

static void compute_stage(hls::stream<int> &input,
                          hls::stream<int> &output)
{
    for (int index = 0; index < N; ++index)
    {
#pragma HLS PIPELINE II=1
        output.write(input.read() * 3 + 1);
    }
}

static void store_stage(hls::stream<int> &values, int output[N])
{
    for (int index = 0; index < N; ++index)
    {
#pragma HLS PIPELINE II=1
        output[index] = values.read();
    }
}

void vector_stream_pipeline_dataflow(const int input[N], int output[N])
{
    hls::stream<int> loaded;
    hls::stream<int> computed;

#pragma HLS STREAM variable=loaded depth=4 type=fifo
#pragma HLS STREAM variable=computed depth=8 type=fifo
#pragma HLS DATAFLOW
    load_stage(input, loaded);
    compute_stage(loaded, computed);
    store_stage(computed, output);
}
