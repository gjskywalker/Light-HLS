#include <hls_stream.h>

static const int SIZE = 32;

__attribute__((noinline)) static void
conv5x5_stage0(int input[SIZE][SIZE], int filter[5][5],
               hls::stream<int> &output)
{
    for (int i = 0; i < SIZE; ++i)
    {
        for (int j = 0; j < SIZE; ++j)
        {
            int value = 0;
            if (i >= 2 && i < SIZE - 2 && j >= 2 && j < SIZE - 2)
            {
                for (int k = -2; k <= 2; ++k)
                {
                    for (int l = -2; l <= 2; ++l)
                    {
                        value += filter[2 + k][2 + l] *
                                 input[i + k][j + l];
                    }
                }
            }
            output.write(value);
        }
    }
}

__attribute__((noinline)) static void
conv5x5_stage1(hls::stream<int> &input, int filter[5][5],
               hls::stream<int> &output)
{
    int values[SIZE][SIZE];

    for (int i = 0; i < SIZE; ++i)
    {
        for (int j = 0; j < SIZE; ++j)
        {
            values[i][j] = input.read();
        }
    }

    for (int i = 0; i < SIZE; ++i)
    {
        for (int j = 0; j < SIZE; ++j)
        {
            int value = 0;
            if (i >= 2 && i < SIZE - 2 && j >= 2 && j < SIZE - 2)
            {
                for (int k = -2; k <= 2; ++k)
                {
                    for (int l = -2; l <= 2; ++l)
                    {
                        value += filter[2 + k][2 + l] *
                                 values[i + k][j + l] * 321;
                    }
                }
                if (value > 0)
                    value *= 23;
            }
            output.write(value);
        }
    }
}

__attribute__((noinline)) static void
conv3x3_stage2(hls::stream<int> &input, int filter[3][3],
               int output[SIZE][SIZE])
{
    int values[SIZE][SIZE];

    for (int i = 0; i < SIZE; ++i)
    {
        for (int j = 0; j < SIZE; ++j)
        {
            values[i][j] = input.read();
        }
    }

    for (int i = 0; i < SIZE; ++i)
    {
        for (int j = 0; j < SIZE; ++j)
        {
            int value = 0;
            if (i >= 1 && i < SIZE - 1 && j >= 1 && j < SIZE - 1)
            {
                for (int k = -1; k <= 1; ++k)
                {
                    for (int l = -1; l <= 1; ++l)
                    {
                        value += filter[1 + k][1 + l] *
                                 values[i + k][j + l];
                    }
                }
            }
            output[i][j] = value;
        }
    }
}

void convs(int input[SIZE][SIZE], int output[SIZE][SIZE],
           int filter2[3][3], int filter1[5][5], int filter0[5][5])
{
    hls::stream<int> mid0_stream;
    hls::stream<int> mid1_stream;

#pragma HLS STREAM variable=mid0_stream depth=64 type=fifo
#pragma HLS STREAM variable=mid1_stream depth=64 type=fifo
#pragma HLS DATAFLOW
    conv5x5_stage0(input, filter0, mid0_stream);
    conv5x5_stage1(mid0_stream, filter1, mid1_stream);
    conv3x3_stage2(mid1_stream, filter2, output);
}
