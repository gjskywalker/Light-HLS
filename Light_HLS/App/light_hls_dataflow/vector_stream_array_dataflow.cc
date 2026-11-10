#include <hls_stream.h>

static void produce(const int input[2][3],
                    hls::stream<int> channels[2][3])
{
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < 3; ++column)
            channels[row][column].write(input[row][column]);
    }
}

static void consume(hls::stream<int> channels[2][3],
                    int output[2][3])
{
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < 3; ++column)
            output[row][column] = channels[row][column].read();
    }
}

void vector_stream_array_dataflow(const int input[2][3],
                                  int output[2][3])
{
    hls::stream<int> channels[2][3];

#pragma HLS STREAM variable=channels depth=4 type=fifo
#pragma HLS DATAFLOW
    produce(input, channels);
    consume(channels, output);
}
