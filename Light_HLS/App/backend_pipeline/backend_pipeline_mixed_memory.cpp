void backend_pipeline_mixed_memory(int memory[16])
{
#pragma HLS INTERFACE mode=ap_memory port=memory storage_type=ram_1p
    for (int i = 0; i < 8; ++i)
    {
#pragma HLS PIPELINE II=1
        memory[i + 8] = memory[i] + 1;
    }
}
