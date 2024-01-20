#include <stdio.h>
#include <stdarg.h>

// nums: 输入的参数个数
int m_sum( int nums, ... )
{ 
    int count = 0, val = 0, sum = 0; 
    va_list maker;
    va_start(maker, nums); 
    while(count < nums) 
    { 
        val = va_arg(maker, int);
        sum += val; 
        count++; 
    }
    va_end(maker);
    return sum; 
}

int main()
{
    printf("%d\n", m_sum(5, 1, 2, 3, 4, 5));
    return 0;
}