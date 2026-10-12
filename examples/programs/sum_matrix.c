/* Sum a matrix row by row, then column by column.
 * Capture a real trace with Valgrind's Lackey tool:
 *   gcc -O1 -g -o sum_matrix sum_matrix.c
 *   valgrind --tool=lackey --trace-mem=yes --log-file=sum_matrix.trc ./sum_matrix
 * Row order walks memory sequentially; column order jumps N*8 bytes per step. */
#include <stdio.h>

#define N 128
static double m[N][N];

int main(void) {
    double row_sum = 0.0, col_sum = 0.0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) m[i][j] = i + j;
    for (int i = 0; i < N; i++)          /* row-major: cache friendly */
        for (int j = 0; j < N; j++) row_sum += m[i][j];
    for (int j = 0; j < N; j++)          /* column-major: one line per access */
        for (int i = 0; i < N; i++) col_sum += m[i][j];
    printf("%f %f\n", row_sum, col_sum);
    return 0;
}
