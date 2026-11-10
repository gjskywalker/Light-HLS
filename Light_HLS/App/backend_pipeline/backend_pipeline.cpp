void backend_pipeline(int input[16], int output[16])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = input[i] + 3;
    }
}
