void backend_pipeline_nested_conflict(int input[16], int output[16])
{
    for (int outer = 0; outer < 4; ++outer)
    {
#pragma HLS PIPELINE II=1
        for (int inner = 0; inner < 4; ++inner)
        {
#pragma HLS PIPELINE II=1
            int index = outer * 4 + inner;
            output[index] = input[index] + 5;
        }
    }
}
