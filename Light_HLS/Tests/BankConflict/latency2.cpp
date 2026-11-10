void qor_pressure(unsigned input[256], unsigned output[256], unsigned coefficient)
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    unsigned tile[256];
#pragma HLS ARRAY_PARTITION variable=tile type=cyclic factor=2 dim=1
#pragma HLS BIND_STORAGE variable=tile type=ram_1p impl=bram latency=2
    for (unsigned i = 0; i < 256; ++i)
    {
#pragma HLS PIPELINE II=1
        tile[i] = input[i] ^ coefficient;
    }
    for (unsigned i = 0; i < 256; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = tile[i] + tile[i ^ 1u];
    }
}
