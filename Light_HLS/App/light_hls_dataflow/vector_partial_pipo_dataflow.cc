static void produce_partial(const int input[3], int stage[4])
{
    for (int index = 0; index < 3; ++index)
        stage[index] = input[index] + 4;
}

static void consume_partial(const int stage[4], int output[3])
{
    for (int index = 0; index < 3; ++index)
        output[index] = stage[index] * 2;
}

void vector_partial_pipo_dataflow(const int input[3], int output[3])
{
    int stage[4];

#pragma HLS DATAFLOW
    produce_partial(input, stage);
    consume_partial(stage, output);
}
