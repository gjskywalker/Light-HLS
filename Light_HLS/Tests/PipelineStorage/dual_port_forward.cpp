void dual_port_forward(unsigned input[96], unsigned output[32])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_2p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    for (unsigned index = 0; index < 32; ++index)
    {
#pragma HLS PIPELINE II=1
        output[index] = input[index] + input[index + 32];
    }
}
