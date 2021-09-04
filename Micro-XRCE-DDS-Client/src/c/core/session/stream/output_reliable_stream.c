#include "output_reliable_stream_internal.h"

#include <uxr/client/config.h>
#include <ucdr/microcdr.h>
#include <string.h>

#include "./seq_num_internal.h"
#include "./output_reliable_stream_internal.h"
#include "./common_reliable_stream_internal.h"
#include "../submessage_internal.h"

#define MIN_HEARTBEAT_TIME_INTERVAL ((int64_t) UXR_CONFIG_MIN_HEARTBEAT_TIME_INTERVAL) // ms
#define MAX_HEARTBEAT_TRIES         (sizeof(int64_t) * 8 - 1)

//========================test=======================
/**
 * 如果两值相等 返回0
 * 2 > 1 返回-1
 * 1 > 2 返回 1
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
    else if ((seq_num_1 < seq_num_2 && (seq_num_2 - seq_num_1) < 1<<15) ||
            (seq_num_1 > seq_num_2 && (seq_num_1 - seq_num_2) > 1<<15))
    {
        result = -1;
    }
    else
    {
        result = 1;
    }
    return result;
}
//==================================================================
//                             PUBLIC
//==================================================================
void uxr_init_output_reliable_stream(
        uxrOutputReliableStream* stream,
        uint8_t* buffer,
        size_t size,
        uint16_t history,
        uint8_t header_offset)
{
    // assert for history (must be 2^)

    stream->base.buffer = buffer;
    stream->base.size = size;
    stream->base.history = history;
    stream->offset = header_offset;

    uxr_reset_output_reliable_stream(stream);
}

void uxr_reset_output_reliable_stream(
        uxrOutputReliableStream* stream)
{
    for (uint16_t i = 0; i < stream->base.history; ++i)
    {
        uxr_set_reliable_buffer_size(&stream->base, i, stream->offset);
    }

    stream->last_written = 0;
    stream->last_sent = SEQ_NUM_MAX;
    stream->last_acknown = SEQ_NUM_MAX;

    stream->next_heartbeat_timestamp = INT64_MAX;
    stream->next_heartbeat_tries = 0;
    stream->send_lost = false;
}

/**
 * 准备可靠buffer来写
 * 
 * */
bool uxr_prepare_reliable_buffer_to_write(
        uxrOutputReliableStream* stream,
        size_t length,
        ucdrBuffer* ub)
{
    bool available_to_write = false;
    uxrSeqNum seq_num = stream->last_written;       // seq_num为last_written
    size_t buffer_capacity = uxr_get_reliable_buffer_capacity(&stream->base);       // 获取容量
    uint8_t* buffer = uxr_get_reliable_buffer(&stream->base, seq_num);      // 获取buffer
    size_t buffer_size = uxr_get_reliable_buffer_size(&stream->base, seq_num);  // 获取大小

    /* Check if the message fit in the current buffer */    // 检查是否message适合当前buffer
    if (buffer_size + length <= buffer_capacity)
    {
        /* Check if there is space in the stream history to write */        // 检查流的历史中是否还有写的空间
        uxrSeqNum last_available = uxr_seq_num_add(stream->last_acknown, stream->base.history); // 最近一次确认的序列号加上历史的大小 最大可获取的
        available_to_write = (0 >= uxr_seq_num_cmp(seq_num, last_available));   // 上次写的==最大可获取的 或者最近写的小于最大可获取的
        if (available_to_write)
        {
            size_t final_buffer_size = buffer_size + length;
            uxr_set_reliable_buffer_size(&stream->base, seq_num, final_buffer_size);    // 设置可靠buffer的大小
            ucdr_init_buffer_origin_offset(ub, buffer, (uint32_t)final_buffer_size, 0u, (uint32_t)buffer_size); // 重新初始化buffer
        }
    }
    /* Check if the message fit in a new empty buffer */    // 否则看message是否适合一个新的buffer
    else if (stream->offset + length <= buffer_capacity)
    {
        /* Check if there is space in the stream history to write */    //检测流历史是否可以写
        seq_num = uxr_seq_num_add(stream->last_written, 1);
        uxrSeqNum last_available = uxr_seq_num_add(stream->last_acknown, stream->base.history);
        available_to_write = (0 >= uxr_seq_num_cmp(seq_num, last_available));
        if (available_to_write)
        {
            buffer = uxr_get_reliable_buffer(&stream->base, seq_num);
            size_t final_buffer_size = stream->offset + length;
            uxr_set_reliable_buffer_size(&stream->base, seq_num, final_buffer_size);
            ucdr_init_buffer_origin_offset(ub, buffer, (uint32_t)final_buffer_size, 0u, stream->offset);
            stream->last_written = seq_num;
        }
    }
    /* Check if the message fit in a fragmented message */
    else            // message适合一个分段消息
    {
        /* Check if the current buffer free space is too small */
        if (buffer_size + (size_t)SUBHEADER_SIZE >= buffer_capacity)    // 检查当前buffer的空闲空间是否太小了
        {
            seq_num = uxr_seq_num_add(seq_num, 1);
            buffer = uxr_get_reliable_buffer(&stream->base, seq_num);
            buffer_size = uxr_get_reliable_buffer_size(&stream->base, seq_num);
        }

        size_t remaining_blocks = get_available_free_slots(stream);
        uint16_t available_block_size = (uint16_t)(buffer_capacity - (uint16_t)(stream->offset + SUBHEADER_SIZE));
        
        uint16_t first_fragment_size = (uint16_t)(buffer_capacity - (uint16_t)(buffer_size + SUBHEADER_SIZE));
        uint16_t remaining_size = (uint16_t)(length - first_fragment_size);
        uint16_t last_fragment_size;
        uint16_t necessary_complete_blocks;
        if (0 == (remaining_size % available_block_size))
        {
            last_fragment_size = available_block_size;
            necessary_complete_blocks = (uint16_t)((remaining_size / available_block_size));
        }
        else
        {
            last_fragment_size = remaining_size % available_block_size;
            necessary_complete_blocks = (uint16_t)((remaining_size / available_block_size) + 1);
        }

        available_to_write = necessary_complete_blocks <= remaining_blocks;
        if (available_to_write)
        {
            ucdrBuffer temp_ub;
            uint16_t fragment_size = first_fragment_size;
            for (uint16_t i = 0; i < necessary_complete_blocks; i++)
            {
                ucdr_init_buffer_origin_offset(
                    &temp_ub,
                    uxr_get_reliable_buffer(&stream->base, seq_num),
                    buffer_capacity,
                    0u,
                    uxr_get_reliable_buffer_size(&stream->base, seq_num));
                uxr_buffer_submessage_header(&temp_ub, SUBMESSAGE_ID_FRAGMENT, fragment_size, 0);
                uxr_set_reliable_buffer_size(&stream->base, seq_num, buffer_capacity);
                seq_num = uxr_seq_num_add(seq_num, 1);
                fragment_size = available_block_size;
            }

            ucdr_init_buffer_origin_offset(
                &temp_ub,
                uxr_get_reliable_buffer(&stream->base, seq_num),
                buffer_capacity,
                0u,
                uxr_get_reliable_buffer_size(&stream->base, seq_num));
            uxr_buffer_submessage_header(&temp_ub, SUBMESSAGE_ID_FRAGMENT, last_fragment_size, FLAG_LAST_FRAGMENT);
            uxr_set_reliable_buffer_size(&stream->base, seq_num,
                    stream->offset + (size_t)(SUBHEADER_SIZE) + last_fragment_size);

            ucdr_init_buffer(
                ub,
                buffer + buffer_size + SUBHEADER_SIZE,
                (uint32_t)(buffer_capacity - buffer_size - SUBHEADER_SIZE));
            ucdr_set_on_full_buffer_callback(ub, on_full_output_buffer, stream);
            stream->last_written = seq_num;
        }
    }

    return available_to_write;
}
/**
 * 准备下次发送可靠buffer
 * 
 * */
bool uxr_prepare_next_reliable_buffer_to_send(
        uxrOutputReliableStream* stream,
        uint8_t** buffer,
        size_t* length,
        uxrSeqNum* seq_num)
{
    // seq_num++
    *seq_num = uxr_seq_num_add(stream->last_sent, 1);
    // 获取要发送的部分
    *buffer = uxr_get_reliable_buffer(&stream->base, *seq_num);
    // 获取要发送部分的大小
    *length = uxr_get_reliable_buffer_size(&stream->base, *seq_num);
    // 进行比对看是否可以发送
    bool data_to_send = 0 >= uxr_seq_num_cmp(*seq_num, stream->last_written)    // last_written比将要send的序号要大或者相等
            && *length > stream->offset     // 偏移量不可以偏移出大小范围
            && uxr_seq_num_sub(stream->last_sent, stream->last_acknown) != stream->base.history;    // 上次发送-上次确认不可以（大于）等于历史，因为再发就大了
    if (data_to_send)
    {
        stream->last_sent = *seq_num;
        if (stream->last_sent == stream->last_written)      //如果sent和written相同，written需要+1
        {
            stream->last_written = uxr_seq_num_add(stream->last_written, 1);
        }
    }

    return data_to_send;
}
/**
 * 更新心跳时间戳
 * */
bool uxr_update_output_stream_heartbeat_timestamp(
        uxrOutputReliableStream* stream,
        int64_t current_timestamp)
{
    bool must_confirm = false;
    // 如果已发送的大于已确认，但是没有超过seq_num的一半，或者说已确认的，大于已发送的，且超过了seq_num的一半
    if (0 > uxr_seq_num_cmp(stream->last_acknown, stream->last_sent))
    {
        // 如果下次心跳尝试为0(第一次心跳尝试)
        if (0 == stream->next_heartbeat_tries)
        {
            // 下次心跳尝试时间戳设置为当前时间+最小心跳间隔（1ms）
            stream->next_heartbeat_timestamp = current_timestamp + MIN_HEARTBEAT_TIME_INTERVAL;
            // 下次心跳尝试设置为1
            stream->next_heartbeat_tries = 1;
        }
        // 否则如果当前时间戳大于等于下次心跳时间戳的话， 
        else if (current_timestamp >= stream->next_heartbeat_timestamp)
        {
            // 增量设置为 时间间隔 左移 尝试次数
            int64_t increment = MIN_HEARTBEAT_TIME_INTERVAL << (stream->next_heartbeat_tries % MAX_HEARTBEAT_TRIES);
            // 当前时间戳-下次心跳时间戳
            int64_t difference = current_timestamp - stream->next_heartbeat_timestamp;
            // 下次心跳时间戳更新为加上两者较大的数
            stream->next_heartbeat_timestamp += (difference > increment) ? difference : increment;
            // 心跳次数增加
            stream->next_heartbeat_tries++;
            // 确认
            must_confirm = true;
        }
    }
    else// 如果已确认的大于等于已发送的，且没超过seq_num的一半，那么就不用发送心跳
    {
        // 设置下次心跳时间戳为一个很大的数
        stream->next_heartbeat_timestamp = INT64_MAX;
    }

    return must_confirm;
}

uxrSeqNum uxr_begin_output_nack_buffer_it(
        const uxrOutputReliableStream* stream)
{
    return stream->last_acknown;
}
/**
 * 下一个可靠的nackbuff准备发送
 * */
bool uxr_next_reliable_nack_buffer_to_send(
        uxrOutputReliableStream* stream,
        uint8_t** buffer,
        size_t* length,
        uxrSeqNum* seq_num_it)
{
    bool it_updated = false;
    // 如果存在sendlost
    if (stream->send_lost)
    {
        bool check_next_buffer = true;  
        while (check_next_buffer && !it_updated)
        {   // 置为last_ack+1
            *seq_num_it = uxr_seq_num_add(*seq_num_it, 1);
            // 如果新的last_ack和stream的last_sent相比小就代表已经全部重新写入buffer了，否则就一条一条写进去。
            // 流中的last_sent大 ，或等于新的seqnum为真
            check_next_buffer = 0 >= uxr_seq_num_cmp(*seq_num_it, stream->last_sent);
            if (check_next_buffer)
            {
                *buffer = uxr_get_reliable_buffer(&stream->base, *seq_num_it);
                *length = uxr_get_reliable_buffer_size(&stream->base, *seq_num_it);
                it_updated = *length != stream->offset; // 检查发送的长度是不是不等于stream的offset
            }
        }
        // 如果此时it_updated为假，说明length和offset已经相等
        if (!it_updated)
        {
            stream->send_lost = false;  // 不再有漏发了。
        }
    }

    return it_updated;
}
/**
 * 
 * */
void uxr_process_acknack(
        uxrOutputReliableStream* stream,
        uint16_t bitmap,
        uxrSeqNum first_unacked_seq_num)
{
    // 获取事实上的last_ack
    uxrSeqNum last_acked_seq_num = uxr_seq_num_sub(first_unacked_seq_num, 1);
    size_t buffers_to_clean = uxr_seq_num_sub(last_acked_seq_num, stream->last_acknown);
    for (size_t i = 0; i < buffers_to_clean; i++)
    {
        // 更新stream中的last_ack
        stream->last_acknown = uxr_seq_num_add(stream->last_acknown, 1);
        uxr_set_reliable_buffer_size(&stream->base, stream->last_acknown, stream->offset);
    }
    // 设置stream中的sendlost 如果0<bitmap,则说明存在丢失，为真，否则为假
    stream->send_lost = (0 < bitmap);
    // 重置stream中的下次心跳次数
    /* reset heartbeat interval */
    stream->next_heartbeat_tries = 0;
}
// 更新流状态，一旦last_sent 和last_ack不相等了，返回false，否则返回true
bool uxr_is_output_up_to_date(
        const uxrOutputReliableStream* stream)
{
    return 0 == uxr_seq_num_cmp(stream->last_acknown, stream->last_sent);
}

//==================================================================
//                             PRIVATE
//==================================================================
bool on_full_output_buffer(
        ucdrBuffer* ub,
        void* args)
{
    uxrOutputReliableStream* stream = (uxrOutputReliableStream*) args;

    uint16_t history_position = (uint16_t)(1 + uxr_get_reliable_buffer_history_position(&stream->base, ub->init));
    uint8_t* buffer = uxr_get_reliable_buffer(&stream->base, history_position);
    size_t buffer_size = uxr_get_reliable_buffer_size(&stream->base, history_position);

    ucdr_init_buffer_origin(
        ub,
        buffer + stream->offset + SUBHEADER_SIZE,
        (uint32_t)(buffer_size - stream->offset - SUBHEADER_SIZE),
        ub->offset);
    ucdr_set_on_full_buffer_callback(ub, on_full_output_buffer, stream);

    return false;
}

uint16_t get_available_free_slots(
        uxrOutputReliableStream* stream)
{
    uint16_t free_slots = 0;
    for (uint16_t i = 0; i < stream->base.history; i++)
    {
        if (uxr_get_reliable_buffer_size(&stream->base, i) == stream->offset)
        {
            free_slots++;
        }
    }
    return free_slots;
}
