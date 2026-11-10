void row_dependent(int input[16], int output[16])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    for (int outer = 0; outer < 4; ++outer)
        for (int inner = 0; inner < 4; ++inner)
        {
#pragma HLS PIPELINE II=1
            int index = outer * 4 + inner;
            output[index] = input[index] + outer;
        }
}
