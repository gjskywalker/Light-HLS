static const int N = 16;

static void pipeline_partition_produce(const int input[N], int stage[N])
{
    for (int index = 0; index < N; ++index)
    {
#pragma HLS PIPELINE II=1
        stage[index] = input[index] + 2;
    }
}

static void pipeline_partition_consume(const int stage[N], int output[N])
{
    for (int index = 0; index < N; ++index)
    {
#pragma HLS PIPELINE II=1
        output[index] = stage[index] * 3;
    }
}

void vector_pipeline_partition_dataflow(const int input[N], int output[N])
{
    int stage[N];

#pragma HLS ARRAY_PARTITION variable=stage type=cyclic factor=4 dim=1
#pragma HLS DATAFLOW
    pipeline_partition_produce(input, stage);
    pipeline_partition_consume(stage, output);
}
