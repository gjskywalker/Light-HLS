#include <hls_stream.h>

static void left_task(hls::stream<int> &right_to_left,
                      hls::stream<int> &left_to_right)
{
    left_to_right.write(right_to_left.read() + 1);
}

static void right_task(hls::stream<int> &left_to_right,
                       hls::stream<int> &right_to_left, int output[1])
{
    int value = left_to_right.read();
    right_to_left.write(value + 1);
    output[0] = value;
}

void invalid_zero_token_cycle(int output[1])
{
    hls::stream<int> left_to_right;
    hls::stream<int> right_to_left;

#pragma HLS STREAM variable=left_to_right depth=2 type=fifo
#pragma HLS STREAM variable=right_to_left depth=2 type=fifo
#pragma HLS DATAFLOW
    left_task(right_to_left, left_to_right);
    right_task(left_to_right, right_to_left, output);
}
