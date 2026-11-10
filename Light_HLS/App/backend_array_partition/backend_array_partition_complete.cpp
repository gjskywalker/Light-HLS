void backend_array_partition_complete(int input[4], int output[4])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
#pragma HLS ARRAY_PARTITION variable=input type=complete dim=1
    output[0] = input[0] + input[1] + input[2] + input[3];
}
