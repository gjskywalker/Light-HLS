int memory_request_order(int memory[8], int seed)
{
#pragma HLS INTERFACE mode=ap_memory port=memory storage_type=ram_2p
    int index = memory[0] & 7;
    int result = memory[index];
    memory[7] = seed;
    return result;
}
