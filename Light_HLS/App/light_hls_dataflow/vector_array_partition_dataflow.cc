static const int N = 64;

static void produce_partitioned(const int input[N], int stage[N])
{
    for (int index = 0; index < N; ++index)
        stage[index] = input[index] * 2;
}

static void consume_partitioned(const int stage[N], int output[N])
{
    for (int index = 0; index < N; ++index)
        output[index] = stage[index] + 3;
}

void vector_array_partition_dataflow(const int input[N], int output[N])
{
    int stage[N];

#pragma HLS ARRAY_PARTITION variable=stage type=cyclic factor=4 dim=1
#pragma HLS DATAFLOW
    produce_partitioned(input, stage);
    consume_partitioned(stage, output);
}
