#define TMAX 40
#define NX 15
#define NY 80

static void update_ey(float ey_input[NX][NY],
                      float ey_output[NX][NY],
                      float hz_input[NX][NY],
                      float ey_stage[NX][NY])
{
    for (int j = 0; j < NY; ++j)
        ey_stage[0][j] = ey_output[0][j];

    for (int i = 1; i < NX; ++i)
    {
        for (int j = 0; j < NY; ++j)
        {
            float value =
                ey_input[i][j] -
                (float)(0.5) * (hz_input[i][j] - hz_input[i - 1][j]);
            ey_stage[i][j] = value;
            ey_output[i][j] = value;
        }
    }
}

static void update_ex(float ex_input[NX][NY],
                      float ex_output[NX][NY],
                      float hz_input[NX][NY],
                      float ex_stage[NX][NY])
{
    for (int i = 0; i < NX; ++i)
        ex_stage[i][0] = ex_output[i][0];

    for (int i = 0; i < NX; ++i)
    {
        for (int j = 1; j < NY; ++j)
        {
            float value =
                ex_input[i][j] -
                (float)(0.5) * (hz_input[i][j] - hz_input[i][j - 1]);
            ex_stage[i][j] = value;
            ex_output[i][j] = value;
        }
    }
}

static void update_hz(float ex_stage[NX][NY],
                      float ey_stage[NX][NY],
                      float hz_input[NX][NY],
                      float hz_output[NX][NY])
{
    for (int i = 0; i < NX - 1; ++i)
    {
        for (int j = 0; j < NY - 1; ++j)
        {
            hz_output[i][j] =
                hz_input[i][j] -
                (float)(0.7) *
                    (ex_stage[i][j + 1] - ex_stage[i][j] +
                     ey_stage[i + 1][j] - ey_stage[i][j]);
        }
    }
}

void kernel_fdtd_2d(int tmax,
                    int nx,
                    int ny,
                    float ex_input[NX][NY],
                    float ex_output[NX][NY],
                    float ey_input[NX][NY],
                    float ey_output[NX][NY],
                    float hz_input[NX][NY],
                    float hz_output[NX][NY])
{
    float ex_stage[NX][NY];
    float ey_stage[NX][NY];

#pragma HLS DATAFLOW
    update_ey(ey_input, ey_output, hz_input, ey_stage);
    update_ex(ex_input, ex_output, hz_input, ex_stage);
    update_hz(ex_stage, ey_stage, hz_input, hz_output);
}
