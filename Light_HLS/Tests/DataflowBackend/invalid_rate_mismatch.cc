#include <hls_stream.h>

static void produce(hls::stream<int> &channel)
{
    for (int index = 0; index < 4; ++index)
        channel.write(index);
}

static void consume(hls::stream<int> &channel, int output[3])
{
    for (int index = 0; index < 3; ++index)
        output[index] = channel.read();
}

void invalid_rate_mismatch(int output[3])
{
    hls::stream<int> channel;

#pragma HLS STREAM variable=channel depth=4 type=fifo
#pragma HLS DATAFLOW
    produce(channel);
    consume(channel, output);
}
