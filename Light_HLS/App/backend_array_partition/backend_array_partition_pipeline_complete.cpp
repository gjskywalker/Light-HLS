void backend_array_partition_pipeline_complete(int input[4], int output[4])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
#pragma HLS ARRAY_PARTITION variable=input type=complete dim=1
    for (int i = 0; i < 4; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = input[i] * 3 + 1;
    }
}
