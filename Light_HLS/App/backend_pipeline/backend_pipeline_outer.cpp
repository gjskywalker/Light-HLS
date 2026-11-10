void backend_pipeline_outer(int input[16], int output[16])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    for (int outer = 0; outer < 4; ++outer)
    {
#pragma HLS PIPELINE II=1
        for (int inner = 0; inner < 4; ++inner)
        {
            int index = outer * 4 + inner;
            output[index] = input[index] + 5;
        }
    }
}
