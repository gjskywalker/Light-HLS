void storage_stress(unsigned input[96], unsigned output[32])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    for (unsigned index = 0; index < 32; ++index)
    {
#pragma HLS PIPELINE II=1
        unsigned a = input[index];
        unsigned b = input[a & 31];
        unsigned c = input[(b & 31) + 32];
        output[index] = a + b + c;
    }
}
