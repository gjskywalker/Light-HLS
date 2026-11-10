#include <hls_stream.h>

static void conditional_fifo_produce(int enabled, hls::stream<int> &values)
{
    if (enabled)
        values.write(41);
}

static void conditional_fifo_consume(int enabled, hls::stream<int> &values,
                                     int output[1])
{
    if (enabled)
        output[0] = values.read() + 1;
    else
        output[0] = 0;
}

void conditional_fifo_dataflow(int enabled, int output[1])
{
    hls::stream<int> values;

#pragma HLS STREAM variable=values depth=2 type=fifo
#pragma HLS DATAFLOW
    conditional_fifo_produce(enabled, values);
    conditional_fifo_consume(enabled, values, output);
}
