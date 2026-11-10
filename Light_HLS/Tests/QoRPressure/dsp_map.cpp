void qor_pressure(unsigned input[256], unsigned output[256], unsigned coefficient)
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    for (unsigned i = 0; i < 256; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = (input[i] & 0x1ffffu) * (coefficient & 0x1ffffu) + i;
    }
}
