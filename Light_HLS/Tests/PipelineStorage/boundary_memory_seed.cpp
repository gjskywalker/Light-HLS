unsigned boundary_memory_seed(unsigned input[32], unsigned bias)
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
    unsigned sum = input[31];
    for (unsigned index = 0; index < 17; ++index)
    {
#pragma HLS PIPELINE II=1
        sum += input[index] + bias;
    }
    return sum;
}
