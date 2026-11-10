void backend_pipeline_memii(int input[32], int output[16])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = input[i] + input[i + 16];
    }
}
