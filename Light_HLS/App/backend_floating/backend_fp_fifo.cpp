#include <hls_stream.h>

static void producer(hls::stream<float> &input_stream, hls::stream<float> &stage)
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        stage.write(input_stream.read());
    }
}

static void consumer(hls::stream<float> &stage, hls::stream<float> &output_stream)
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        output_stream.write(__builtin_sqrtf(stage.read()) / 3.0f + 0.25f);
    }
}

void dut(hls::stream<float> &input_stream, hls::stream<float> &output_stream)
{
    hls::stream<float> stage;
#pragma HLS STREAM variable=input_stream depth=4 type=fifo bitwidth=32
#pragma HLS STREAM variable=output_stream depth=4 type=fifo bitwidth=32
#pragma HLS STREAM variable=stage depth=2 type=fifo
#pragma HLS DATAFLOW
    producer(input_stream, stage);
    consumer(stage, output_stream);
}
