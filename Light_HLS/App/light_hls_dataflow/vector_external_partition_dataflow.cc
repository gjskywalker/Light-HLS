#include <hls_stream.h>

static const int N = 64;

void load_partitioned(const int input[N], hls::stream<int> &values)
{
#pragma HLS ARRAY_PARTITION variable=input type=cyclic factor=4 dim=1
    for (int index = 0; index < N; ++index)
        values.write(input[index]);
}

void store_partitioned(hls::stream<int> &values, int output[N])
{
    for (int index = 0; index < N; ++index)
        output[index] = values.read() + 5;
}

void vector_external_partition_dataflow(const int input[N], int output[N])
{
    hls::stream<int> values;

#pragma HLS STREAM variable=values depth=4 type=fifo
#pragma HLS DATAFLOW
    load_partitioned(input, values);
    store_partitioned(values, output);
}
