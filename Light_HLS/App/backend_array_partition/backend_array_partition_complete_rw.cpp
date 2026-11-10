void backend_array_partition_complete_rw(int data[4])
{
#pragma HLS INTERFACE mode=ap_memory port=data storage_type=ram_1p
#pragma HLS ARRAY_PARTITION variable=data type=complete dim=1
    for (int i = 0; i < 4; ++i)
        data[i] = data[i] * 3 + 1;
}
