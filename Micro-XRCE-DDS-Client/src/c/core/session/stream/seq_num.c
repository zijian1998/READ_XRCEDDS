#include "seq_num_internal.h"

#include <stdint.h>
#define SEQ_NUM_SIZE     (1 << 16)
#define SEQ_NUM_MIDSIZE  (SEQ_NUM_SIZE >> 1)

//==================================================================
//                             PUBLIC
//==================================================================
uxrSeqNum uxr_seq_num_add(
        uxrSeqNum seq_num,
        uint16_t increment)
{
    return (uxrSeqNum)((seq_num + increment) % SEQ_NUM_SIZE);
}

uxrSeqNum uxr_seq_num_sub(
        uxrSeqNum seq_num,
        uint16_t decrement)
{
    return (uxrSeqNum)((decrement > seq_num)
        ? seq_num + (SEQ_NUM_SIZE - decrement)
        : seq_num - decrement);
}
/**
 * 序列号比较，
 * a-b==0 0
 * a-b>0 并且 b-a<c/2 或者a-b>0并且a-n>c/2  -1
 * 
 * 如果序列号相等返回0
 * 如果1<2并且2-1<最大序列号的一半 或者 1>2并且2-1》最大序列号的一般返回-1
 * 否则返回1
 * */
int uxr_seq_num_cmp(
        uxrSeqNum seq_num_1,
        uxrSeqNum seq_num_2)
{
    int result;
    if (seq_num_1 == seq_num_2)
    {
        result = 0;
    }
    else if ((seq_num_1 < seq_num_2 && (seq_num_2 - seq_num_1) < SEQ_NUM_MIDSIZE) ||
            (seq_num_1 > seq_num_2 && (seq_num_1 - seq_num_2) > SEQ_NUM_MIDSIZE))
    {
        result = -1;
    }
    else
    {
        result = 1;
    }
    return result;
}
