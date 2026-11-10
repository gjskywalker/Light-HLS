void backend_array_partition_local(int input[16], int output[4])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    int local[16];
#pragma HLS ARRAY_PARTITION variable=local type=cyclic factor=4 dim=1

    for (int i = 0; i < 16; ++i)
        local[i] = input[i] + 1;

    for (int i = 0; i < 4; ++i)
    {
        output[i] = local[4 * i] + local[4 * i + 1] +
                    local[4 * i + 2] + local[4 * i + 3];
    }
}
