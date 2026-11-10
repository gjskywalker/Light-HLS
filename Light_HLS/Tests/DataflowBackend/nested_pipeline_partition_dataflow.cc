static void nested_produce(const int input[16], int stage[16])
{
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
#pragma HLS PIPELINE II=1
            int index = row * 4 + column;
            stage[index] = input[index] + 2;
        }
}

static void nested_consume(const int stage[16], int output[16])
{
    for (int index = 0; index < 16; ++index)
    {
#pragma HLS PIPELINE II=1
        output[index] = stage[index] * 3;
    }
}

void vector_pipeline_partition_dataflow(const int input[16], int output[16])
{
    int stage[16];
#pragma HLS ARRAY_PARTITION variable=stage type=cyclic factor=4 dim=1
#pragma HLS DATAFLOW
    nested_produce(input, stage);
    nested_consume(stage, output);
}
