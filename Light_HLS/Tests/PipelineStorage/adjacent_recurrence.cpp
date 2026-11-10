unsigned adjacent_recurrence(unsigned input[32], unsigned bias, unsigned seed)
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
    unsigned sum = seed;
    for (unsigned index = 0; index < 17; ++index)
    {
#pragma HLS PIPELINE II=1
        sum += input[index] + bias;
    }
    for (unsigned index = 0; index < 17; ++index)
    {
#pragma HLS PIPELINE II=1
        sum += input[index] ^ bias;
    }
    return sum;
}
