#include <hls_stream.h>

static const int N = 16;

static void broadcast_producer(const int input[N], hls::stream<int> &values)
{
    for (int index = 0; index < N; ++index)
        values.write(input[index]);
}

static void broadcast_left(hls::stream<int> &values, int output[N])
{
    for (int index = 0; index < N; ++index)
        output[index] = values.read() + 1;
}

static void broadcast_right(hls::stream<int> &values, int output[N])
{
    for (int index = 0; index < N; ++index)
        output[index] = values.read() * 2;
}

void vector_broadcast_dataflow(const int input[N], int left[N], int right[N])
{
    hls::stream<int> values;

#pragma HLS STREAM variable=values depth=4 type=fifo
#pragma HLS DATAFLOW
    broadcast_producer(input, values);
    broadcast_left(values, left);
    broadcast_right(values, right);
}
