// Copyright 2017-present Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef UXR__CLIENT__CORE__SESSION__STREAM__OUTPUT_RELIABLE_STREAM_H_
#define UXR__CLIENT__CORE__SESSION__STREAM__OUTPUT_RELIABLE_STREAM_H_

#ifdef __cplusplus
extern "C"
{
#endif // ifdef __cplusplus

#include <uxr/client/core/session/stream/reliable_stream.h>
#include <uxr/client/core/session/stream/seq_num.h>

#include <stdbool.h>

struct ucdrBuffer;
struct uxrOutputReliableStream;

typedef void (* OnNewFragment)(
        struct ucdrBuffer* ub,
        struct uxrOutputReliableStream* stream);

typedef struct uxrOutputReliableStream
{
    uxrReliableStream base;     // 
    uint8_t offset;

    uxrSeqNum last_written;     // 上次写的序号
    uxrSeqNum last_sent;        // 上次发送的序号
    uxrSeqNum last_acknown;     // 上次确认的序号

    int64_t next_heartbeat_timestamp;       // 下次发送心跳的时间
    uint8_t next_heartbeat_tries;           // 下次发送心跳的尝试次数
    bool send_lost;                         // 是否有丢失

} uxrOutputReliableStream;

#ifdef __cplusplus
}
#endif // ifdef __cplusplus

#endif // UXR__CLIENT__CORE__SESSION__STREAM__OUTPUT_RELIABLE_STREAM_H_
