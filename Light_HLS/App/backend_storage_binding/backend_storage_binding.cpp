void backend_storage_binding(int input[16], int output[16])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    int bram_buffer[16];
    int uram_buffer[16];
    int lutram_buffer[16];
#pragma HLS BIND_STORAGE variable=bram_buffer type=ram_1p impl=bram latency=2
#pragma HLS BIND_STORAGE variable=uram_buffer type=ram_2p impl=uram latency=2
#pragma HLS BIND_STORAGE variable=lutram_buffer type=ram_1p impl=lutram latency=1

    for (int i = 0; i < 16; ++i)
        bram_buffer[i] = input[i] + 1;
    for (int i = 0; i < 16; ++i)
        uram_buffer[i] = bram_buffer[i] * 3;
    for (int i = 0; i < 16; ++i)
        lutram_buffer[i] = uram_buffer[i] - 2;
    for (int i = 0; i < 16; ++i)
        output[i] = lutram_buffer[i] + i;
}
