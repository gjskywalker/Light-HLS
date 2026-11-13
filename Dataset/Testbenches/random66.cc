/*
+--------------------------------------------------------------------------+
| CHStone : a suite of benchmark programs for C-based High-Level Synthesis |
| ======================================================================== |
|                                                                          |
| * Collected and Modified : Y. Hara, H. Tomiyama, S. Honda,               |
|                            H. Takada and K. Ishii                        |
|                            Nagoya University, Japan                      |
|                                                                          |
| * Remark :                                                               |
|    1. This source code is modified to unify the formats of the benchmark |
|       programs in CHStone.                                               |
|    2. Test vectors are added for CHStone.                                |
|    3. If "main_result" is 0 at the end of the program, the program is    |
|       correctly executed.                                                |
|    4. Please follow the copyright of each benchmark program.             |
+--------------------------------------------------------------------------+
*/
/*============================================================================

This C header file is part of the SoftFloat IEC/IEEE Floating-point Arithmetic
Package, Release 2b.

Written by John R. Hauser.  This work was made possible in part by the
International Computer Science Institute, located at Suite 600, 1947 Center
Street, Berkeley, California 94704.  Funding was partially provided by the
National Science Foundation under grant MIP-9311980.  The original version
of this code was written as part of a project to build a fixed-point vector
processor in collaboration with the University of California at Berkeley,
overseen by Profs. Nelson Morgan and John Wawrzynek.  More information
is available through the Web page `http://www.cs.berkeley.edu/~jhauser/
arithmetic/SoftFloat.html'.

THIS SOFTWARE IS DISTRIBUTED AS IS, FOR FREE.  Although reasonable effort has
been made to avoid it, THIS SOFTWARE MAY CONTAIN FAULTS THAT WILL AT TIMES
RESULT IN INCORRECT BEHAVIOR.  USE OF THIS SOFTWARE IS RESTRICTED TO PERSONS
AND ORGANIZATIONS WHO CAN AND WILL TAKE FULL RESPONSIBILITY FOR ALL LOSSES,
COSTS, OR OTHER PROBLEMS THEY INCUR DUE TO THE SOFTWARE, AND WHO FURTHERMORE
EFFECTIVELY INDEMNIFY JOHN HAUSER AND THE INTERNATIONAL COMPUTER SCIENCE
INSTITUTE (possibly via similar legal warning) AGAINST ALL LOSSES, COSTS, OR
OTHER PROBLEMS INCURRED BY THEIR CUSTOMERS AND CLIENTS DUE TO THE SOFTWARE.

Derivative works are acceptable, even for commercial purposes, so long as
(1) the source code for the derivative work includes prominent notice that
the work is derivative, and (2) the source code includes prominent notice with
these four paragraphs for those parts of this code that are retained.

=============================================================================*/

/*----------------------------------------------------------------------------
| Software IEC/IEEE floating-point types.
*----------------------------------------------------------------------------*/
typedef unsigned int float32;
typedef unsigned long long float64;

/*----------------------------------------------------------------------------
| Software IEC/IEEE floating-point underflow tininess-detection mode.
*----------------------------------------------------------------------------*/
#define float_tininess_after_rounding   0
#define float_tininess_before_rounding  1

/*----------------------------------------------------------------------------
| Software IEC/IEEE floating-point rounding mode.
*----------------------------------------------------------------------------*/
#define float_round_nearest_even  0
#define float_round_to_zero       1
#define float_round_up            2
#define float_round_down          3

/*----------------------------------------------------------------------------
| Software IEC/IEEE floating-point exception flags.
*----------------------------------------------------------------------------*/
#define float_flag_inexact     1
#define float_flag_divbyzero   2
#define float_flag_underflow   4
#define float_flag_overflow    8
#define float_flag_invalid    16

#define N 46
const float64 a_input[N] = {
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x4000000000000000ULL,	/* 2.0 */
  0x4000000000000000ULL,	/* 2.0 */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x0000000000000000ULL,	/* 0.0 */
  0x3FF8000000000000ULL,	/* 1.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x0000000000000000ULL,	/* 0.0 */
  0x3FF8000000000000ULL,	/* 1.5 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0xC000000000000000ULL,	/* -2.0 */
  0xC000000000000000ULL,	/* -2.0 */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xBFF0000000000000ULL,	/* -1.0 */
  0x8000000000000000ULL,	/* -0.0 */
  0xBFF8000000000000ULL,	/* -1.5 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0x8000000000000000ULL,	/* -0.0 */
  0xBFF8000000000000ULL,	/* -1.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x0000000000000000ULL,	/* 0.0 */
  0x3FF8000000000000ULL,	/* 1.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x4000000000000000ULL,	/* 2.0 */
  0xFFF0000000000000ULL,	/* -inf */
  0xFFF0000000000000ULL,	/* -inf */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xBFF0000000000000ULL,	/* -1.0 */
  0x8000000000000000ULL,	/* -0.0 */
  0xBFF8000000000000ULL,	/* -1.5 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xC000000000000000ULL		/* -2.0 */
};

const float64 b_input[N] = {
  0x3FF0000000000000ULL,	/* 1.0 */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x0000000000000000ULL,	/* 0.0 */
  0x3FF8000000000000ULL,	/* 1.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x4000000000000000ULL,	/* 2.0 */
  0x4000000000000000ULL,	/* 2.0 */
  0x7FF0000000000000ULL,	/* inf */
  0x7FF0000000000000ULL,	/* inf */
  0x0000000000000000ULL,	/* 0.0 */
  0x3FF0000000000000ULL,	/* 1.0 */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xBFF0000000000000ULL,	/* -1.0 */
  0x8000000000000000ULL,	/* -0.0 */
  0xBFF8000000000000ULL,	/* -1.5 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0xC000000000000000ULL,	/* -2.0 */
  0xC000000000000000ULL,	/* -2.0 */
  0xFFF0000000000000ULL,	/* -inf */
  0xFFF0000000000000ULL,	/* -inf */
  0x8000000000000000ULL,	/* -inf */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xFFF0000000000000ULL,	/* -inf */
  0xFFF0000000000000ULL,	/* -inf */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xC000000000000000ULL,	/* -2.0 */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xBFF0000000000000ULL,	/* -1.0 */
  0x8000000000000000ULL,	/* -0.0 */
  0xBFF8000000000000ULL,	/* -1.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x4000000000000000ULL,	/* 2.0 */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x0000000000000000ULL,	/* 0.0 */
  0x3FF8000000000000ULL		/* 1.5 */
};

const float64 z_output[N] = {
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x4000000000000000ULL,	/* 2.0 */
  0x400C000000000000ULL,	/* 3.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x4000000000000000ULL,	/* 2.0 */
  0x400C000000000000ULL,	/* 3.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x0000000000000000ULL,	/* 0.0 */
  0x4004000000000000ULL,	/* 2.5 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0xC000000000000000ULL,	/* -2.0 */
  0xC00C000000000000ULL,	/* -3.5 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0xC000000000000000ULL,	/* -2.0 */
  0xC00C000000000000ULL,	/* -3.5 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0x8000000000000000ULL,	/* -0.0 */
  0xC004000000000000ULL,	/* -2.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FFFFFFFFFFFFFFFULL,	/* nan */
  0x0000000000000000ULL,	/* 0.0 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xBFE0000000000000ULL,	/* -0.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x3FE0000000000000ULL,	/* 0.5 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FFFFFFFFFFFFFFFULL,	/* nan */
  0x0000000000000000ULL,	/* 0.0 */
  0x7FF8000000000000ULL,	/* nan */
  0x7FF0000000000000ULL,	/* inf */
  0x3FF0000000000000ULL,	/* 1.0 */
  0x3FE0000000000000ULL,	/* 0.5 */
  0xFFF8000000000000ULL,	/* nan */
  0xFFF0000000000000ULL,	/* -inf */
  0xBFF0000000000000ULL,	/* -1.0 */
  0xBFE0000000000000ULL		/* -0.5 */
};
typedef int flag;
typedef int int8;
typedef int int16;

/*----------------------------------------------------------------------------
| Each of the following `typedef's defines a type that holds integers
| of _exactly_ the number of bits specified.  For instance, for most
| implementation of C, `bits16' and `sbits16' should be `typedef'ed to
| `unsigned short int' and `signed short int' (or `short int'), respectively.
*----------------------------------------------------------------------------*/
typedef unsigned short int bits16;
typedef unsigned int bits32;
typedef unsigned long long int bits64;
typedef signed long long int sbits64;

/*----------------------------------------------------------------------------
| The `LIT64' macro takes as its argument a textual integer literal and
| if necessary ``marks'' the literal as having a 64-bit integer type.
| For example, the GNU C Compiler (`gcc') requires that 64-bit literals be
| appended with the letters `LL' standing for `long long', which is `gcc's
| name for the 64-bit integer type.  Some compilers may allow `LIT64' to be
| defined as the identity macro:  `#define LIT64( a ) a'.
*----------------------------------------------------------------------------*/
#define LIT64( a ) a##LL
int8 float_exception_flags = 0;
// Replacement arithmetic for the truncated benchmark. Integer datapaths retain
// the software-FP workload; rounding is nearest-even with gradual underflow.
static bits64 shift_right_jam(bits64 value, unsigned distance)
{
    if (distance == 0)
        return value;
    if (distance >= 64)
        return value != 0;
    return (value >> distance) | ((value << (64 - distance)) != 0);
}

static float64 add_binary64(float64 a, float64 b)
{
    const bits64 fraction_mask = 0x000FFFFFFFFFFFFFULL;
    const bits64 quiet_bit = 0x0008000000000000ULL;
    unsigned sign_a = a >> 63, sign_b = b >> 63;
    unsigned exp_a = (a >> 52) & 0x7FF, exp_b = (b >> 52) & 0x7FF;
    bits64 sig_a = a & fraction_mask, sig_b = b & fraction_mask;
    const bool nan_a = exp_a == 0x7FF && sig_a != 0;
    const bool nan_b = exp_b == 0x7FF && sig_b != 0;
    if (nan_a || nan_b)
    {
        if ((nan_a && !(sig_a & quiet_bit)) || (nan_b && !(sig_b & quiet_bit)))
            float_exception_flags |= float_flag_invalid;
        return (nan_b ? b : a) | quiet_bit;
    }
    if (exp_a == 0x7FF || exp_b == 0x7FF)
    {
        if (exp_a == 0x7FF && exp_b == 0x7FF && sign_a != sign_b)
        {
            float_exception_flags |= float_flag_invalid;
            return 0x7FFFFFFFFFFFFFFFULL;
        }
        return exp_a == 0x7FF ? a : b;
    }
    if (exp_a) sig_a |= 1ULL << 52;
    else exp_a = 1;
    if (exp_b) sig_b |= 1ULL << 52;
    else exp_b = 1;
    // Put the larger magnitude first so opposite-sign subtraction is unsigned.
    if (exp_a < exp_b || (exp_a == exp_b && sig_a < sig_b))
    {
        unsigned temp_exp = exp_a; exp_a = exp_b; exp_b = temp_exp;
        unsigned temp_sign = sign_a; sign_a = sign_b; sign_b = temp_sign;
        bits64 temp_sig = sig_a; sig_a = sig_b; sig_b = temp_sig;
    }
    sig_a <<= 3;
    sig_b = shift_right_jam(sig_b << 3, exp_a - exp_b);
    bits64 sum;
    if (sign_a == sign_b)
    {
        sum = sig_a + sig_b;
        if (sum & (1ULL << 56))
        {
            sum = shift_right_jam(sum, 1);
            ++exp_a;
        }
    }
    else
    {
        sum = sig_a - sig_b;
        if (sum == 0)
            return 0;
        for (int shift = 0; shift < 55; ++shift)
            if (!(sum & (1ULL << 55)) && exp_a > 1)
            {
                sum <<= 1;
                --exp_a;
            }
    }
    const unsigned round_bits = sum & 7;
    bits64 rounded = sum >> 3;
    if (round_bits > 4 || (round_bits == 4 && (rounded & 1)))
        ++rounded;
    if (rounded & (1ULL << 53))
    {
        rounded >>= 1;
        ++exp_a;
    }
    if (exp_a >= 0x7FF)
    {
        float_exception_flags |= float_flag_overflow | float_flag_inexact;
        return ((bits64)sign_a << 63) | 0x7FF0000000000000ULL;
    }
    const unsigned packed_exp = (rounded & (1ULL << 52)) ? exp_a : 0;
    if (round_bits)
    {
        float_exception_flags |= float_flag_inexact;
        if (!packed_exp)
            float_exception_flags |= float_flag_underflow;
    }
    return ((bits64)sign_a << 63) | ((bits64)packed_exp << 52) |
           (rounded & fraction_mask);
}

int dut ()
{
    int main_result = 0;
    float_exception_flags = 0;
    for (int i = 0; i < N; ++i)
    {
        const float64 result = add_binary64(a_input[i], b_input[i]);
        main_result += result != z_output[i];
    }
    return main_result;
}
