void backend_pipeline_memory_dependence(int memory[16], int output[15])
{
#pragma HLS INTERFACE mode=ap_memory port=memory storage_type=ram_1p
#pragma HLS INTERFACE mode=ap_memory port=output storage_type=ram_1p
    for (int i = 0; i < 15; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = memory[i + 1];
        memory[i] = i + 100;
    }
}
