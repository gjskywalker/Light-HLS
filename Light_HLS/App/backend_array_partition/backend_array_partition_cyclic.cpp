void backend_array_partition_cyclic(int input[16], int output[4])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
#pragma HLS ARRAY_PARTITION variable=input type=cyclic factor=4 dim=1
    for (int i = 0; i < 4; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = input[4 * i] + input[4 * i + 1] +
                    input[4 * i + 2] + input[4 * i + 3];
    }
}
