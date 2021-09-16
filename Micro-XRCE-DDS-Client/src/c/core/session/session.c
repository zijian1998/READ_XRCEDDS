#include <uxr/client/core/session/session.h>
#include <uxr/client/util/time.h>
#include <uxr/client/core/communication/communication.h>
#include <uxr/client/core/type/xrce_types.h>
#include <uxr/client/config.h>

#include "submessage_internal.h"
#include "session_internal.h"
#include "session_info_internal.h"
#include "stream/stream_storage_internal.h"
#include "stream/common_reliable_stream_internal.h"
#include "stream/input_best_effort_stream_internal.h"
#include "stream/input_reliable_stream_internal.h"
#include "stream/output_best_effort_stream_internal.h"
#include "stream/output_reliable_stream_internal.h"
#include "stream/seq_num_internal.h"
#include "../log/log_internal.h"
#include "../../util/time_internal.h"

#define CREATE_SESSION_MAX_MSG_SIZE (MAX_HEADER_SIZE + SUBHEADER_SIZE + CREATE_CLIENT_PAYLOAD_SIZE)
#define DELETE_SESSION_MAX_MSG_SIZE (MAX_HEADER_SIZE + SUBHEADER_SIZE + DELETE_CLIENT_PAYLOAD_SIZE)
#define HEARTBEAT_MAX_MSG_SIZE      (MAX_HEADER_SIZE + SUBHEADER_SIZE + HEARTBEAT_PAYLOAD_SIZE)
#define ACKNACK_MAX_MSG_SIZE        (MAX_HEADER_SIZE + SUBHEADER_SIZE + ACKNACK_PAYLOAD_SIZE)
#define TIMESTAMP_PAYLOAD_SIZE      8
#define TIMESTAMP_MAX_MSG_SIZE      (MAX_HEADER_SIZE + SUBHEADER_SIZE + TIMESTAMP_PAYLOAD_SIZE)

static bool listen_message(
        uxrSession* session,
        int poll_ms);
static bool listen_message_reliably(
        uxrSession* session,
        int poll_ms);

static bool wait_session_status(
        uxrSession* session,
        uint8_t* buffer,
        size_t length,
        size_t attempts);

static bool send_message(
        const uxrSession* session,
        uint8_t* buffer,
        size_t length);
static bool recv_message(
        const uxrSession* session,
        uint8_t** buffer,
        size_t* length,
        int poll_ms);

static void write_submessage_heartbeat(
        const uxrSession* session,
        uxrStreamId stream);
static void write_submessage_acknack(
        const uxrSession* session,
        uxrStreamId stream);

static void read_message(
        uxrSession* session,
        ucdrBuffer* message);
static void read_stream(
        uxrSession* session,
        ucdrBuffer* message,
        uxrStreamId id,
        uxrSeqNum seq_num);
static void read_submessage_list(
        uxrSession* session,
        ucdrBuffer* submessages,
        uxrStreamId stream_id);
static void read_submessage(
        uxrSession* session,
        ucdrBuffer* submessage,
        uint8_t submessage_id,
        uxrStreamId stream_id,
        uint16_t length,
        uint8_t flags);

static void read_submessage_status(
        uxrSession* session,
        ucdrBuffer* submessage);
static void read_submessage_data(
        uxrSession* session,
        ucdrBuffer* submessage,
        uint16_t length,
        uxrStreamId stream_id,
        uint8_t format);
static void read_submessage_heartbeat(
        uxrSession* session,
        ucdrBuffer* submessage);
static void read_submessage_acknack(
        uxrSession* session,
        ucdrBuffer* submessage);
static void read_submessage_timestamp_reply(
        uxrSession* session,
        ucdrBuffer* submessage);
#ifdef PERFORMANCE_TESTING
static void read_submessage_performance(
        uxrSession* session,
        ucdrBuffer* submessage,
        uint16_t length);
#endif /* ifdef PERFORMANCE_TESTING */

static void process_status(
        uxrSession* session,
        uxrObjectId object_id,
        uint16_t request_id,
        uint8_t status);
static void process_timestamp_reply(
        uxrSession* session,
        TIMESTAMP_REPLY_Payload* timestamp);

static FragmentationInfo on_get_fragmentation_info(
        uint8_t* submessage_header);

static bool run_session_until_sync(
        uxrSession* session,
        int timeout);

//==================================================================
//                             PUBLIC
//==================================================================

void uxr_init_session(
        uxrSession* session,
        uxrCommunication* comm,
        uint32_t key)
{
    session->comm = comm;       // 

    session->request_list = NULL;
    session->status_list = NULL;
    session->request_status_list_size = 0;

    session->on_status = NULL;
    session->on_status_args = NULL;
    session->on_topic = NULL;
    session->on_topic_args = NULL;

    session->on_time = NULL;
    session->on_time_args = NULL;
    session->time_offset = 0;
    session->synchronized = false;
    // 初始化session_info，这个里面包括了sessionid（81）、以及key还有最后一次request的id以及状态
    uxr_init_session_info(&session->info, 0x81, key);
    // 初始化stream的storage
    uxr_init_stream_storage(&session->streams);
}

void uxr_set_status_callback(
        uxrSession* session,
        uxrOnStatusFunc on_status_func,
        void* args)
{
    session->on_status = on_status_func;
    session->on_status_args = args;
}

void uxr_set_topic_callback(
        uxrSession* session,
        uxrOnTopicFunc on_topic_func,
        void* args)
{
    session->on_topic = on_topic_func;
    session->on_topic_args = args;
}

void uxr_set_time_callback(
        uxrSession* session,
        uxrOnTimeFunc on_time_func,
        void* args)
{
    session->on_time = on_time_func;
    session->on_time_args = args;
}

void uxr_set_request_callback(
        uxrSession* session,
        uxrOnRequestFunc on_request_func,
        void* args)
{
    session->on_request = on_request_func;
    session->on_request_args = args;
}

void uxr_set_reply_callback(
        uxrSession* session,
        uxrOnReplyFunc on_reply_func,
        void* args)
{
    session->on_reply = on_reply_func;
    session->on_reply_args = args;
}

#ifdef PERFORMANCE_TESTING
void uxr_set_performance_callback(
        uxrSession* session,
        uxrOnPerformanceFunc on_echo_func,
        void* args)
{
    session->on_performance = on_echo_func;
    session->on_performance_args = args;
}

#endif /* ifdef PERFORMANCE_TESTING */
// 创建session
bool uxr_create_session_retries(
        uxrSession* session,
        size_t retries)
{
    //
    uxr_reset_stream_storage(&session->streams);
    // 创建一个buffer
    uint8_t create_session_buffer[CREATE_SESSION_MAX_MSG_SIZE];
    ucdrBuffer ub;
    ucdr_init_buffer_origin_offset(&ub, create_session_buffer, CREATE_SESSION_MAX_MSG_SIZE, 0u, uxr_session_header_offset(
                &session->info));

    uxr_buffer_create_session(&session->info, &ub, (uint16_t)(session->comm->mtu - INTERNAL_RELIABLE_BUFFER_OFFSET));
    uxr_stamp_create_session_header(&session->info, ub.init);

    bool received = wait_session_status(session, create_session_buffer, ucdr_buffer_length(&ub), (size_t) retries);
    bool created = received && UXR_STATUS_OK == session->info.last_requested_status;
    return created;
}
// 创建seesion
bool uxr_create_session(
        uxrSession* session)
{
    return uxr_create_session_retries(session, UXR_CONFIG_MAX_SESSION_CONNECTION_ATTEMPTS);
}

bool uxr_delete_session_retries(
        uxrSession* session,
        size_t retries)
{
    uint8_t delete_session_buffer[DELETE_SESSION_MAX_MSG_SIZE];
    ucdrBuffer ub;
    ucdr_init_buffer_origin_offset(&ub, delete_session_buffer, DELETE_SESSION_MAX_MSG_SIZE, 0u, uxr_session_header_offset(
                &session->info));

    uxr_buffer_delete_session(&session->info, &ub);
    uxr_stamp_session_header(&session->info, 0, 0, ub.init);

    bool received = wait_session_status(session, delete_session_buffer, ucdr_buffer_length(&ub), retries);
    return received && UXR_STATUS_OK == session->info.last_requested_status;
}

bool uxr_delete_session(
        uxrSession* session)
{
    return uxr_delete_session_retries(session, UXR_CONFIG_MAX_SESSION_CONNECTION_ATTEMPTS);
}

uxrStreamId uxr_create_output_best_effort_stream(
        uxrSession* session,
        uint8_t* buffer,
        size_t size)
{
    uint8_t header_offset = uxr_session_header_offset(&session->info);
    return uxr_add_output_best_effort_buffer(&session->streams, buffer, size, header_offset);
}

uxrStreamId uxr_create_output_reliable_stream(
        uxrSession* session,
        uint8_t* buffer,
        size_t size,
        uint16_t history)
{
    uint8_t header_offset = uxr_session_header_offset(&session->info);
    return uxr_add_output_reliable_buffer(&session->streams, buffer, size, history, header_offset);
}

uxrStreamId uxr_create_input_best_effort_stream(
        uxrSession* session)
{
    return uxr_add_input_best_effort_buffer(&session->streams);
}

uxrStreamId uxr_create_input_reliable_stream(
        uxrSession* session,
        uint8_t* buffer,
        size_t size,
        uint16_t history)
{
    return uxr_add_input_reliable_buffer(&session->streams, buffer, size, history, on_get_fragmentation_info);
}
/**
 * 保持客户端和代理的通信
 * 包括两部分：1。 刷新所有通过传输发送数据的输出流
 *          2. 侦听来自Agent的消息，并调用相关的回调
 * 循环执行，直到等待时间超过了timeout_ms
 * */
bool uxr_run_session_time(
        uxrSession* session,
        int timeout_ms)
{
    // 刷新所有通过传输发送数据的数据流
    uxr_flash_output_streams(session);

    bool timeout = false;
    // 循环直到收到消息
    while (!timeout)
    {
        timeout = !listen_message_reliably(session, timeout_ms);
    }
    // 保证每个流的last_sent=last=ack
    return uxr_output_streams_confirmed(&session->streams);
}

bool uxr_run_session_timeout(
        uxrSession* session,
        int timeout_ms)
{
    int64_t start_timestamp = uxr_millis();
    int remaining_time = timeout_ms;

    uxr_flash_output_streams(session);

    while (remaining_time > 0)
    {
        listen_message_reliably(session, remaining_time);
        remaining_time = timeout_ms - (int)(uxr_millis() - start_timestamp);
    }
    return uxr_output_streams_confirmed(&session->streams);
}

bool uxr_run_session_until_data(
        uxrSession* session,
        int timeout_ms)
{
    int64_t start_timestamp = uxr_millis();
    int remaining_time = timeout_ms;

    uxr_flash_output_streams(session);

    session->on_data_flag = false;
    while (remaining_time > 0)
    {
        listen_message_reliably(session, remaining_time);
        if (session->on_data_flag)
        {
            break;
        }
        remaining_time = timeout_ms - (int)(uxr_millis() - start_timestamp);
    }
    return session->on_data_flag;
}

bool uxr_run_session_until_timeout(
        uxrSession* session,
        int timeout_ms)
{
    uxr_flash_output_streams(session);

    return listen_message_reliably(session, timeout_ms);
}

bool uxr_run_session_until_confirm_delivery(
        uxrSession* session,
        int timeout_ms)
{
    uxr_flash_output_streams(session);

    bool timeout = false;
    while (!uxr_output_streams_confirmed(&session->streams) && !timeout)
    {
        timeout = !listen_message_reliably(session, timeout_ms);
    }

    return uxr_output_streams_confirmed(&session->streams);
}

bool uxr_run_session_until_all_status(
        uxrSession* session,
        int timeout_ms,
        const uint16_t* request_list,
        uint8_t* status_list,
        size_t list_size)
{
    uxr_flash_output_streams(session);

    for (unsigned i = 0; i < list_size; ++i)
    {
        status_list[i] = UXR_STATUS_NONE;
    }

    session->request_list = request_list;
    session->status_list = status_list;
    session->request_status_list_size = list_size;

    bool timeout = false;
    bool status_confirmed = false;
    while (!timeout && !status_confirmed)
    {
        timeout = !listen_message_reliably(session, timeout_ms);
        status_confirmed = true;
        for (unsigned i = 0; i < list_size && status_confirmed; ++i)
        {
            status_confirmed = status_list[i] != UXR_STATUS_NONE
                    || request_list[i] == UXR_INVALID_REQUEST_ID;         //CHECK: better give an error? an assert?
        }
    }

    session->request_status_list_size = 0;

    bool status_ok = true;
    for (unsigned i = 0; i < list_size && status_ok; ++i)
    {
        status_ok = status_list[i] == UXR_STATUS_OK || status_list[i] == UXR_STATUS_OK_MATCHED;
    }

    return status_ok;
}

bool uxr_run_session_until_one_status(
        uxrSession* session,
        int timeout_ms,
        const uint16_t* request_list,
        uint8_t* status_list,
        size_t list_size)
{
    uxr_flash_output_streams(session);

    for (unsigned i = 0; i < list_size; ++i)
    {
        status_list[i] = UXR_STATUS_NONE;
    }

    session->request_list = request_list;
    session->status_list = status_list;
    session->request_status_list_size = list_size;

    bool timeout = false;
    bool status_confirmed = false;
    while (!timeout && !status_confirmed)
    {
        timeout = !listen_message_reliably(session, timeout_ms);
        for (unsigned i = 0; i < list_size && !status_confirmed; ++i)
        {
            status_confirmed = status_list[i] != UXR_STATUS_NONE
                    || request_list[i] == UXR_INVALID_REQUEST_ID;         //CHECK: better give an error? an assert?
        }
    }

    session->request_status_list_size = 0;

    return status_confirmed;
}

bool uxr_sync_session(
        uxrSession* session,
        int time)
{
    uint8_t timestamp_buffer[TIMESTAMP_MAX_MSG_SIZE];
    ucdrBuffer ub;
    ucdr_init_buffer_origin_offset(&ub, timestamp_buffer, sizeof(timestamp_buffer), 0u,
            uxr_session_header_offset(&session->info));
    uxr_buffer_submessage_header(&ub, SUBMESSAGE_ID_TIMESTAMP, TIMESTAMP_PAYLOAD_SIZE, 0);

    TIMESTAMP_Payload timestamp;
    int64_t nanos = uxr_nanos();
    timestamp.transmit_timestamp.seconds = (int32_t)(nanos / 1000000000);
    timestamp.transmit_timestamp.nanoseconds = (uint32_t)(nanos % 1000000000);
    (void) uxr_serialize_TIMESTAMP_Payload(&ub, &timestamp);

    uxr_stamp_session_header(&session->info, 0, 0, ub.init);
    send_message(session, timestamp_buffer, ucdr_buffer_length(&ub));
    return run_session_until_sync(session, time);
}

int64_t uxr_epoch_millis(
        uxrSession* session)
{
    return uxr_epoch_nanos(session) / 1000000;
}

int64_t uxr_epoch_nanos(
        uxrSession* session)
{
    return uxr_nanos() - session->time_offset;
}

#ifdef PERFORMANCE_TESTING
bool uxr_buffer_performance(
        uxrSession* session,
        uxrStreamId stream_id,
        uint64_t epoch_time,
        uint8_t* buf,
        uint16_t len,
        bool echo)
{
    bool rv = false;
    PERFORMANCE_Payload payload;
    payload.epoch_time_lsb = (uint32_t)(epoch_time & UINT32_MAX);
    payload.epoch_time_msb = (uint32_t)(epoch_time >> 32);
    payload.buf = buf;
    payload.len = len;
    ucdrBuffer mb;
    const uint16_t payload_length = (uint16_t)(sizeof(payload.epoch_time_lsb) +
            sizeof(payload.epoch_time_msb) +
            len);

    uint8_t flags = (echo) ? UXR_ECHO : 0;
    if (uxr_prepare_stream_to_write_submessage(session, stream_id, payload_length, &mb, SUBMESSAGE_ID_PERFORMANCE,
            flags))
    {
        (void) uxr_serialize_PERFORMANCE_Payload(&mb, &payload);
        rv = true;
    }
    return rv;
}

#endif /* ifdef PERFORMANCE_TESTING */
/**
 * 将session中的所有流都进行发送，既包括尽力而为传输流，又包括可靠传输流
 * */
void uxr_flash_output_streams(
        uxrSession* session)
{
    // 尽力而为传输流
    for (uint8_t i = 0; i < session->streams.output_best_effort_size; ++i)
    {
        uxrOutputBestEffortStream* stream = &session->streams.output_best_effort[i];
        // 创建流标识符
        uxrStreamId id = uxr_stream_id(i, UXR_BEST_EFFORT_STREAM, UXR_OUTPUT_STREAM);

        uint8_t* buffer; size_t length; uxrSeqNum seq_num;
        // 准备发送的buffer
        if (uxr_prepare_best_effort_buffer_to_send(stream, &buffer, &length, &seq_num))
        {
            uxr_stamp_session_header(&session->info, id.raw, seq_num, buffer);
            send_message(session, buffer, length);
        }
    }
    // 可靠传输流
    for (uint8_t i = 0; i < session->streams.output_reliable_size; ++i)
    {
        // session->streams是所有流的汇总的结构体，里面有4个数组，数组组成元素才是stream
        uxrOutputReliableStream* stream = &session->streams.output_reliable[i];
        // 创建流标识符streamid
        uxrStreamId id = uxr_stream_id(i, UXR_RELIABLE_STREAM, UXR_OUTPUT_STREAM);

        uint8_t* buffer; size_t length; uxrSeqNum seq_num;
        // 准备要发送的buffer
        while (uxr_prepare_next_reliable_buffer_to_send(stream, &buffer, &length, &seq_num))
        {
            uxr_stamp_session_header(&session->info, id.raw, seq_num, buffer);
            send_message(session, buffer, length);
        }
    }
}

//==================================================================
//                             PRIVATE
//==================================================================
bool listen_message(
        uxrSession* session,
        int poll_ms)
{
    uint8_t* data; size_t length;
    bool must_be_read = recv_message(session, &data, &length, poll_ms);
    if (must_be_read)
    {
        ucdrBuffer ub;
        ucdr_init_buffer(&ub, data, (uint32_t)length);
        read_message(session, &ub);
    }

    return must_be_read;
}
/**
 * 监听可靠消息
 * @param:session
 * @param:超时时限，单位是毫秒
 * */
bool  listen_message_reliably(
        uxrSession* session,
        int poll_ms)
{
    bool received = false;
    // 计时器清零重新进行倒数
    int32_t poll = (poll_ms >= 0) ? poll_ms : INT32_MAX;
    do
    {
        // 下次心跳时间戳，初始化为一个很大的数
        int64_t next_heartbeat_timestamp = INT64_MAX;
        // 当前时间戳
        int64_t timestamp = uxr_millis();
        // 循环所有的可靠传输流
        for (uint8_t i = 0; i < session->streams.output_reliable_size; ++i)
        {
            uxrOutputReliableStream* stream = &session->streams.output_reliable[i];
            // 创建stream_id
            uxrStreamId id = uxr_stream_id(i, UXR_RELIABLE_STREAM, UXR_OUTPUT_STREAM);
            // 更新心跳时间戳
            if (uxr_update_output_stream_heartbeat_timestamp(stream, timestamp))
            {
                // 写心跳子消息
                write_submessage_heartbeat(session, id);
            }
            // 如果stream中的下次心跳时间戳小于了下次心跳时间戳
            if (stream->next_heartbeat_timestamp < next_heartbeat_timestamp)
            {
                //  就把心跳时间戳改为strem当中的下次心跳时间戳
                next_heartbeat_timestamp = stream->next_heartbeat_timestamp;
            }
        }
        // 下次心跳时长赋值为poll（如果未经过上面的初始化）或者stream中的下次心跳时间戳-当前时间戳
        int32_t poll_to_next_heartbeat =
                (next_heartbeat_timestamp != INT64_MAX) ? (int32_t)(next_heartbeat_timestamp - timestamp) : poll;
        if (0 == poll_to_next_heartbeat)
        {
            poll_to_next_heartbeat = 1;
        }

        int poll_chosen = (poll_to_next_heartbeat < poll) ? poll_to_next_heartbeat : poll;
        // 监听是否收到了消息，此值为真，说明收到了，且对漏发的消息重新进行了发送
        received = listen_message(session, poll_chosen);
        if (!received)
        {
            poll -= poll_chosen;
        }
    }while (!received && poll > 0);

    return received;
}

bool wait_session_status(
        uxrSession* session,
        uint8_t* buffer,
        size_t length,
        size_t attempts)
{
    session->info.last_requested_status = UXR_STATUS_NONE;

    for (size_t i = 0; i < attempts && session->info.last_requested_status == UXR_STATUS_NONE; ++i)
    {
        send_message(session, buffer, length);
        listen_message(session, UXR_CONFIG_MIN_SESSION_CONNECTION_INTERVAL);
    }

    return session->info.last_requested_status != UXR_STATUS_NONE;
}
/**
 * 内联函数，调用session中的communacation中的指向发送消息的函数的函数指针
 * 并返回发送结果
 * */
inline bool send_message(
        const uxrSession* session,
        uint8_t* buffer,
        size_t length)
{
    bool sent = session->comm->send_msg(session->comm->instance, buffer, length);
    UXR_DEBUG_PRINT_MESSAGE((sent) ? UXR_SEND : UXR_ERROR_SEND, buffer, length, session->info.key);
    return sent;
}

inline bool recv_message(
        const uxrSession* session,
        uint8_t** buffer,
        size_t* length,
        int poll_ms)
{
    bool received = session->comm->recv_msg(session->comm->instance, buffer, length, poll_ms);
    if (received)
    {
        UXR_DEBUG_PRINT_MESSAGE(UXR_RECV, *buffer, *length, session->info.key);
    }
    return received;
}
/**
 * 写心跳子消息
 * */
void write_submessage_heartbeat(
        const uxrSession* session,
        uxrStreamId id)
{
    uint8_t heartbeat_buffer[HEARTBEAT_MAX_MSG_SIZE];
    ucdrBuffer ub;
    // 初始化buffer的origin和offset
    ucdr_init_buffer_origin_offset(&ub, heartbeat_buffer, HEARTBEAT_MAX_MSG_SIZE, 0u,
            uxr_session_header_offset(&session->info));
    // 可靠流
    const uxrOutputReliableStream* stream = &session->streams.output_reliable[id.index];

    /* Buffer submessage header. */
    // 填充子消息头的buffer
    uxr_buffer_submessage_header(&ub, SUBMESSAGE_ID_HEARTBEAT, HEARTBEAT_PAYLOAD_SIZE, 0);

    /* Buffer HEARTBEAT. */
    // 填充payload，包括最近确认消息和最后发送消息
    HEARTBEAT_Payload payload;
    payload.first_unacked_seq_nr = uxr_seq_num_add(stream->last_acknown, 1);
    payload.last_unacked_seq_nr = stream->last_sent;
    payload.stream_id = id.raw;
    (void) uxr_serialize_HEARTBEAT_Payload(&ub, &payload);  // 序列化

    /* Stamp message header. */
    uxr_stamp_session_header(&session->info, 0, 0, ub.init);
    send_message(session, heartbeat_buffer, ucdr_buffer_length(&ub));       // 发送
}

void write_submessage_acknack(
        const uxrSession* session,
        uxrStreamId id)
{
    uint8_t acknack_buffer[ACKNACK_MAX_MSG_SIZE];
    ucdrBuffer ub;
    ucdr_init_buffer_origin_offset(&ub, acknack_buffer, ACKNACK_MAX_MSG_SIZE, 0u,
            uxr_session_header_offset(&session->info));

    const uxrInputReliableStream* stream = &session->streams.input_reliable[id.index];

    /* Buffer submessage header. */
    uxr_buffer_submessage_header(&ub, SUBMESSAGE_ID_ACKNACK, ACKNACK_PAYLOAD_SIZE, 0);

    /* Buffer ACKNACK. */
    ACKNACK_Payload payload;
    uint16_t nack_bitmap = uxr_compute_acknack(stream, &payload.first_unacked_seq_num);
    payload.nack_bitmap[0] = (uint8_t)(nack_bitmap >> 8);
    payload.nack_bitmap[1] = (uint8_t)((nack_bitmap << 8) >> 8);
    payload.stream_id = id.raw;
    (void) uxr_serialize_ACKNACK_Payload(&ub, &payload);

    /* Stamp message header. */
    uxr_stamp_session_header(&session->info, 0, 0, ub.init);
    send_message(session, acknack_buffer, ucdr_buffer_length(&ub));
}

void read_message(
        uxrSession* session,
        ucdrBuffer* ub)
{
    uint8_t stream_id_raw; uxrSeqNum seq_num;
    if (uxr_read_session_header(&session->info, ub, &stream_id_raw, &seq_num))
    {
        uxrStreamId id = uxr_stream_id_from_raw(stream_id_raw, UXR_INPUT_STREAM);
        read_stream(session, ub, id, seq_num);
    }
}

void read_stream(
        uxrSession* session,
        ucdrBuffer* ub,
        uxrStreamId stream_id,
        uxrSeqNum seq_num)
{
    switch (stream_id.type)
    {
        case UXR_NONE_STREAM:
        {
            stream_id = uxr_stream_id_from_raw(0x00, UXR_INPUT_STREAM);
            read_submessage_list(session, ub, stream_id);
            break;
        }
        case UXR_BEST_EFFORT_STREAM:
        {
            uxrInputBestEffortStream* stream = uxr_get_input_best_effort_stream(&session->streams, stream_id.index);
            if (stream && uxr_receive_best_effort_message(stream, seq_num))
            {
                read_submessage_list(session, ub, stream_id);
            }
            break;
        }
        case UXR_RELIABLE_STREAM:
        {
            uxrInputReliableStream* stream = uxr_get_input_reliable_stream(&session->streams, stream_id.index);
            bool input_buffer_used;
            if (stream &&
                    uxr_receive_reliable_message(stream, seq_num, ub->iterator, ucdr_buffer_remaining(
                        ub), &input_buffer_used))
            {
                if (!input_buffer_used)
                {
                    read_submessage_list(session, ub, stream_id);
                }

                ucdrBuffer next_mb;
                while (uxr_next_input_reliable_buffer_available(stream, &next_mb, SUBHEADER_SIZE))
                {
                    read_submessage_list(session, &next_mb, stream_id);
                }
            }
            write_submessage_acknack(session, stream_id);
            break;
        }
        default:
            break;
    }
}

void read_submessage_list(
        uxrSession* session,
        ucdrBuffer* submessages,
        uxrStreamId stream_id)
{
    uint8_t id; uint16_t length; uint8_t flags;
    while (uxr_read_submessage_header(submessages, &id, &length, &flags))
    {
        read_submessage(session, submessages, id, stream_id, length, flags);
    }
}

void read_submessage(
        uxrSession* session,
        ucdrBuffer* submessage,
        uint8_t submessage_id,
        uxrStreamId stream_id,
        uint16_t length,
        uint8_t flags)
{
    switch (submessage_id)
    {
        case SUBMESSAGE_ID_STATUS_AGENT:
            if (stream_id.type == UXR_NONE_STREAM)
            {
                uxr_read_create_session_status(&session->info, submessage);
            }
            break;

        case SUBMESSAGE_ID_STATUS:
            if (stream_id.type == UXR_NONE_STREAM)
            {
                uxr_read_delete_session_status(&session->info, submessage);
            }
            else
            {
                read_submessage_status(session, submessage);
            }
            break;

        case SUBMESSAGE_ID_DATA:
            read_submessage_data(session, submessage, length, stream_id, flags & FORMAT_MASK);
            break;

        case SUBMESSAGE_ID_HEARTBEAT:
            read_submessage_heartbeat(session, submessage);
            break;

        case SUBMESSAGE_ID_ACKNACK:
            read_submessage_acknack(session, submessage);
            break;

        case SUBMESSAGE_ID_TIMESTAMP_REPLY:
            read_submessage_timestamp_reply(session, submessage);
            break;

#ifdef PERFORMANCE_TESTING
        case SUBMESSAGE_ID_PERFORMANCE:
            read_submessage_performance(session, submessage, length);
            break;
#endif /* ifdef PERFORMANCE_TESTING */

        default:
            break;
    }
}

void read_submessage_status(
        uxrSession* session,
        ucdrBuffer* submessage)
{
    STATUS_Payload payload;
    uxr_deserialize_STATUS_Payload(submessage, &payload);

    uxrObjectId object_id; uint16_t request_id;
    uxr_parse_base_object_request(&payload.base.related_request, &object_id, &request_id);

    uint8_t status = payload.base.result.status;
    process_status(session, object_id, request_id, status);
}

extern void read_submessage_format(
        uxrSession* session,
        ucdrBuffer* data,
        uint16_t length,
        uint8_t format,
        uxrStreamId stream_id,
        uxrObjectId object_id,
        uint16_t request_id);

void read_submessage_data(
        uxrSession* session,
        ucdrBuffer* submessage,
        uint16_t length,
        uxrStreamId stream_id,
        uint8_t format)
{
    BaseObjectRequest base;
    uxr_deserialize_BaseObjectRequest(submessage, &base);
    length = (uint16_t)(length - 4); //CHANGE: by a future size_of_BaseObjectRequest

    uxrObjectId object_id;
    uint16_t request_id;
    uxr_parse_base_object_request(&base, &object_id, &request_id);

    process_status(session, object_id, request_id, UXR_STATUS_OK);
    read_submessage_format(session, submessage, length, format, stream_id, object_id, request_id);
}

void read_submessage_heartbeat(
        uxrSession* session,
        ucdrBuffer* submessage)
{
    HEARTBEAT_Payload heartbeat;
    uxr_deserialize_HEARTBEAT_Payload(submessage, &heartbeat);
    uxrStreamId id = uxr_stream_id_from_raw(heartbeat.stream_id, UXR_INPUT_STREAM);

    uxrInputReliableStream* stream = uxr_get_input_reliable_stream(&session->streams, id.index);
    if (stream)
    {
        uxr_process_heartbeat(stream, heartbeat.first_unacked_seq_nr, heartbeat.last_unacked_seq_nr);
        write_submessage_acknack(session, id);
    }
}
/**
 * 读取acknack子消息
 * */
void read_submessage_acknack(
        uxrSession* session,
        ucdrBuffer* submessage)
{
    ACKNACK_Payload acknack;
    // 反序列化
    uxr_deserialize_ACKNACK_Payload(submessage, &acknack);
    uxrStreamId id = uxr_stream_id_from_raw(acknack.stream_id, UXR_INPUT_STREAM);
    // 拿到可靠输出流
    uxrOutputReliableStream* stream = uxr_get_output_reliable_stream(&session->streams, id.index);
    if (stream)
    {   // 从acknack消息中得到nack的位图
        uint16_t nack_bitmap = (uint16_t)(((uint16_t)acknack.nack_bitmap[0] << 8) + acknack.nack_bitmap[1]);
        // 分析acknack，修改stream信息
        uxr_process_acknack(stream, nack_bitmap, acknack.first_unacked_seq_num);

        uint8_t* buffer; size_t length;
        // 设置seq_num_it为stream中的last_ack，此时last_ack已经更新
        uxrSeqNum seq_num_it = uxr_begin_output_nack_buffer_it(stream);
        // 从上面得到的last_ack开始一条一条重新写进去，一条条重新发。
        while (uxr_next_reliable_nack_buffer_to_send(stream, &buffer, &length, &seq_num_it))
        {
            // 发送出去
            send_message(session, buffer, length);
        }
    }
}

void read_submessage_timestamp_reply(
        uxrSession* session,
        ucdrBuffer* submessage)
{
    TIMESTAMP_REPLY_Payload timestamp_reply;
    uxr_deserialize_TIMESTAMP_REPLY_Payload(submessage, &timestamp_reply);

    process_timestamp_reply(session, &timestamp_reply);
}

#ifdef PERFORMANCE_TESTING
void read_submessage_performance(
        uxrSession* session,
        ucdrBuffer* submessage,
        uint16_t length)
{
    ucdrBuffer mb_performance;
    ucdr_init_buffer(&mb_performance, submessage->iterator, length);
    session->on_performance(session, &mb_performance, session->on_performance_args);
}

#endif /* ifdef PERFORMANCE_TESTING */

void process_status(
        uxrSession* session,
        uxrObjectId object_id,
        uint16_t request_id,
        uint8_t status)
{
    if (session->on_status != NULL)
    {
        session->on_status(session, object_id, request_id, status, session->on_status_args);
    }

    for (unsigned i = 0; i < session->request_status_list_size; ++i)
    {
        if (request_id == session->request_list[i])
        {
            session->status_list[i] = status;
            break;
        }
    }
}

void process_timestamp_reply(
        uxrSession* session,
        TIMESTAMP_REPLY_Payload* timestamp)
{
    if (session->on_time != NULL)
    {
        session->on_time(session,
                uxr_nanos(),
                uxr_convert_to_nanos(timestamp->receive_timestamp.seconds, timestamp->receive_timestamp.nanoseconds),
                uxr_convert_to_nanos(timestamp->transmit_timestamp.seconds, timestamp->transmit_timestamp.nanoseconds),
                uxr_convert_to_nanos(timestamp->originate_timestamp.seconds,
                timestamp->originate_timestamp.nanoseconds),
                session->on_time_args);
    }
    else
    {
        int64_t t3 = uxr_nanos();
        int64_t t0 = uxr_convert_to_nanos(timestamp->originate_timestamp.seconds,
                        timestamp->originate_timestamp.nanoseconds);
        int64_t t1 = uxr_convert_to_nanos(timestamp->receive_timestamp.seconds,
                        timestamp->receive_timestamp.nanoseconds);
        int64_t t2 = uxr_convert_to_nanos(timestamp->transmit_timestamp.seconds,
                        timestamp->transmit_timestamp.nanoseconds);
        session->time_offset = ((t0 + t3) - (t1 + t2)) / 2;
    }
    session->synchronized = true;
}
/**
 * 准备子消息的流
 * */
bool uxr_prepare_stream_to_write_submessage(
        uxrSession* session,
        uxrStreamId stream_id,
        size_t payload_size,
        ucdrBuffer* ub,
        uint8_t submessage_id,
        uint8_t mode)
{
    bool available = false; 
    size_t submessage_size = SUBHEADER_SIZE + payload_size + uxr_submessage_padding(payload_size);      // 子消息大小进行填充

    switch (stream_id.type) // 选择stream种类
    {
        case UXR_BEST_EFFORT_STREAM:
        {
            uxrOutputBestEffortStream* stream = uxr_get_output_best_effort_stream(&session->streams, stream_id.index);
            available = stream && uxr_prepare_best_effort_buffer_to_write(stream, submessage_size, ub);
            break;
        }
        case UXR_RELIABLE_STREAM:
        {
            uxrOutputReliableStream* stream = uxr_get_output_reliable_stream(&session->streams, stream_id.index);   // 获取流
            available = stream && uxr_prepare_reliable_buffer_to_write(stream, submessage_size, ub);                // 准备要写的可靠buffer
            break;
        }
        default:
            break;
    }

    if (available)  // 如果可以获得stream
    {
        (void) uxr_buffer_submessage_header(ub, submessage_id, (uint16_t)payload_size, mode);   // 设置submessage_header
    }

    return available;
}
// 获取fragementation信息
FragmentationInfo on_get_fragmentation_info(
        uint8_t* submessage_header)
{
    ucdrBuffer ub;
    // 初始化buffer
    ucdr_init_buffer(&ub, submessage_header, SUBHEADER_SIZE);

    uint8_t id; uint16_t length; uint8_t flags;
    uxr_read_submessage_header(&ub, &id, &length, &flags);

    FragmentationInfo fragmentation_info;
    if (SUBMESSAGE_ID_FRAGMENT == id)
    {
        fragmentation_info = FLAG_LAST_FRAGMENT & flags ? LAST_FRAGMENT : INTERMEDIATE_FRAGMENT;
    }
    else
    {
        fragmentation_info = NO_FRAGMENTED;
    }
    return fragmentation_info;
}

bool run_session_until_sync(
        uxrSession* session,
        int timeout)
{
    session->synchronized = false;
    bool timeout_exceeded = false;
    while (!timeout_exceeded && !session->synchronized)
    {
        timeout_exceeded = !listen_message_reliably(session, timeout);
    }
    return session->synchronized;
}
