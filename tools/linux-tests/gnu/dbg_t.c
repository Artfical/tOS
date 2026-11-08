#include <stdio.h>
int square(int n)
{
    return n * n;
}
int main(void)
{
    int total = 0;
    for (int i = 1; i <= 3; i++)
        total += square(i);
    printf("total=%d\n", total);
    return 0;
}
