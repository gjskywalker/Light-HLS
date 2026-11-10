static const int N = 8;

static void produce_complete(const int input[N], int stage[N])
{
    for (int index = 0; index < N; ++index)
        stage[index] = input[index] - 2;
}

static void consume_complete(const int stage[N], int output[N])
{
    for (int index = 0; index < N; ++index)
        output[index] = stage[index] * 3;
}

void vector_complete_partition_dataflow(const int input[N], int output[N])
{
    int stage[N];

#pragma HLS ARRAY_PARTITION variable=stage type=complete dim=1
#pragma HLS DATAFLOW
    produce_complete(input, stage);
    consume_complete(stage, output);
}
