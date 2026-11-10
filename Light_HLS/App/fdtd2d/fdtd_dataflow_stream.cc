#include <hls_stream.h>

#define TMAX 40
#define NX 15
#define NY 80

static void update_ey_stream(float ey_input[NX][NY],
                             float ey_output[NX][NY],
                             float hz_input[NX][NY],
                             hls::stream<float> &ey_channel)
{
    for (int j = 0; j < NY; ++j)
        ey_channel.write(ey_output[0][j]);

    for (int i = 1; i < NX; ++i)
    {
        for (int j = 0; j < NY; ++j)
        {
            float value =
                ey_input[i][j] -
                (float)(0.5) * (hz_input[i][j] - hz_input[i - 1][j]);
            ey_output[i][j] = value;
            ey_channel.write(value);
        }
    }
}

static void update_ex_stream(float ex_input[NX][NY],
                             float ex_output[NX][NY],
                             float hz_input[NX][NY],
                             hls::stream<float> &ex_channel)
{
    for (int i = 0; i < NX; ++i)
    {
        ex_channel.write(ex_output[i][0]);
        for (int j = 1; j < NY; ++j)
        {
            float value =
                ex_input[i][j] -
                (float)(0.5) * (hz_input[i][j] - hz_input[i][j - 1]);
            ex_output[i][j] = value;
            ex_channel.write(value);
        }
    }
}

static void update_hz_stream(hls::stream<float> &ex_channel,
                             hls::stream<float> &ey_channel,
                             float hz_input[NX][NY],
                             float hz_output[NX][NY])
{
    float ex_stage[NX][NY];
    float ey_stage[NX][NY];

    for (int i = 0; i < NX; ++i)
        for (int j = 0; j < NY; ++j)
            ex_stage[i][j] = ex_channel.read();

    for (int i = 0; i < NX; ++i)
        for (int j = 0; j < NY; ++j)
            ey_stage[i][j] = ey_channel.read();

    for (int i = 0; i < NX - 1; ++i)
    {
        for (int j = 0; j < NY - 1; ++j)
        {
            hz_output[i][j] =
                hz_input[i][j] -
                (float)(0.7) *
                    (ex_stage[i][j + 1] - ex_stage[i][j] +
                     ey_stage[i + 1][j] - ey_stage[i][j]);
        }
    }
}

void kernel_fdtd_2d(int tmax,
                    int nx,
                    int ny,
                    float ex_input[NX][NY],
                    float ex_output[NX][NY],
                    float ey_input[NX][NY],
                    float ey_output[NX][NY],
                    float hz_input[NX][NY],
                    float hz_output[NX][NY])
{
    hls::stream<float> ex_channel;
    hls::stream<float> ey_channel;

#pragma HLS STREAM variable=ex_channel depth=16 type=fifo
#pragma HLS STREAM variable=ey_channel depth=16 type=fifo
#pragma HLS DATAFLOW
    update_ey_stream(ey_input, ey_output, hz_input, ey_channel);
    update_ex_stream(ex_input, ex_output, hz_input, ex_channel);
    update_hz_stream(ex_channel, ey_channel, hz_input, hz_output);
}
