int backend_pipeline_recurrence(int input[16])
{
#pragma HLS INTERFACE mode=ap_memory port=input storage_type=ram_1p
    int sum = 0;
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        sum += input[i];
    }
    return sum;
}
