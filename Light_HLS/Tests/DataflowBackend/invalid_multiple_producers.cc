#include <hls_stream.h>

static void produce_first(hls::stream<int> &channel)
{
    channel.write(1);
}

static void produce_second(hls::stream<int> &channel)
{
    channel.write(2);
}

static void consume(hls::stream<int> &channel, int output[1])
{
    output[0] = channel.read();
}

void invalid_multiple_producers(int output[1])
{
    hls::stream<int> channel;

#pragma HLS STREAM variable=channel depth=2 type=fifo
#pragma HLS DATAFLOW
    produce_first(channel);
    produce_second(channel);
    consume(channel, output);
}
