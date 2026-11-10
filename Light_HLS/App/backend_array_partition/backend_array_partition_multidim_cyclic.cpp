void backend_array_partition_multidim_cyclic(int input[2][4], int output[2])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
#pragma HLS ARRAY_PARTITION variable=input type=cyclic factor=2 dim=2
    for (int i = 0; i < 2; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = input[i][0] + input[i][1] + input[i][2] + input[i][3];
    }
}
