void signed_chain(int input[16], int output[16])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    for (int index = 0; index < 16; ++index)
    {
#pragma HLS PIPELINE II=1
        short narrow = static_cast<short>(input[index]);
        int extended = narrow;
        output[index] = extended < 0 ? (extended >> 2) : extended * 3;
    }
}
