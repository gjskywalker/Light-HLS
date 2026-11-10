#include <stdio.h>

int backend_scalar(int a, int b, int mode);

int main(void)
{
    static const int cases[][3] = {
        {5, 2, 0},
        {7, 3, 1},
        {9, 4, 2},
        {11, 6, 3},
    };

    for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
    {
        const int a = cases[index][0];
        const int b = cases[index][1];
        const int mode = cases[index][2];
        printf("RESULT %d %d %d %d\n", a, b, mode,
               backend_scalar(a, b, mode));
    }
    return 0;
}
