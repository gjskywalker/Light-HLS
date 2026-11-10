#include <hls_stream.h>

static const int N = 16;

static void public_fifo_transform(hls::stream<int> &input_stream,
                                  hls::stream<int> &stage)
{
    for (int index = 0; index < N; ++index)
        stage.write(input_stream.read() * 2 + 1);
}

static void public_fifo_store(hls::stream<int> &stage,
                              hls::stream<int> &output_stream)
{
    for (int index = 0; index < N; ++index)
        output_stream.write(stage.read() + 3);
}

void public_fifo_dataflow(hls::stream<int> &input_stream,
                          hls::stream<int> &output_stream)
{
    hls::stream<int> stage;

#pragma HLS STREAM variable=input_stream depth=4 type=fifo bitwidth=32
#pragma HLS STREAM variable=output_stream depth=4 type=fifo bitwidth=32
#pragma HLS STREAM variable=stage depth=2 type=fifo
#pragma HLS DATAFLOW
    public_fifo_transform(input_stream, stage);
    public_fifo_store(stage, output_stream);
}
