// Copyright 2017 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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

#include "HelloWorld.h"

#include <uxr/client/client.h>
#include <ucdr/microcdr.h>

#include <stdio.h> //printf
#include <string.h> //strcmp
#include <stdlib.h> //atoi

#define STREAM_HISTORY  8
#define BUFFER_SIZE     UXR_CONFIG_UDP_TRANSPORT_MTU* STREAM_HISTORY

int main(
        int args,
        char** argv)
{
    // CLI
    if (3 > args || 0 == atoi(argv[2]))
    {
        printf("usage: program [-h | --help] | ip port [<max_topics>]\n");
        return 0;
    }
 
    // 将3个命令行参数转换为ip、port和topic
    char* ip = argv[1];
    char* port = argv[2];
    uint32_t max_topics = (args == 4) ? (uint32_t)atoi(argv[3]) : UINT32_MAX;

    // Transport UDP传送初始化
    uxrUDPTransport transport;
    // 根据平台初始化UDP传输 主要是communacation对象的初始化，包括自身实例、发送接受函数、最大传输单元、错误函数等等 
    if (!uxr_init_udp_transport(&transport, UXR_IPv4, ip, port))
    {
        printf("Error at create transport.\n");
        return 1;
    }
    // 创建 Session
    // Session
    uxrSession session;
    // 初始化session，首先是初始化各种成员变量，然后是设置key
    uxr_init_session(&session, &transport.comm, 0xAAAABBBB);
    // 创建session
    if (!uxr_create_session(&session))
    {
        printf("Error at create session.\n");
        return 1;
    }

    // Streams
    // 声明一个可靠的输出stream的buffer
    uint8_t output_reliable_stream_buffer[BUFFER_SIZE];
    // 创建一个可靠的输出stream，包含有
    uxrStreamId reliable_out = uxr_create_output_reliable_stream(&session, output_reliable_stream_buffer, BUFFER_SIZE,
                    STREAM_HISTORY);
    // 声明一个可靠的输入stream的buffer
    uint8_t input_reliable_stream_buffer[BUFFER_SIZE];
    // 创建一个可靠的输入stream的buffer
    uxr_create_input_reliable_stream(&session, input_reliable_stream_buffer, BUFFER_SIZE, STREAM_HISTORY);

    // Create entities
    // 创建实体 participant
    uxrObjectId participant_id = uxr_object_id(0x01, UXR_PARTICIPANT_ID);
    const char* participant_xml = "<dds>"
            "<participant>"
            "<rtps>"
            "<name>default_xrce_participant</name>"
            "</rtps>"
            "</participant>"
            "</dds>";
    // 
    uint16_t participant_req = uxr_buffer_create_participant_xml(&session, reliable_out, participant_id, 0,
                    participant_xml, UXR_REPLACE);
    // 创建topic
    uxrObjectId topic_id = uxr_object_id(0x01, UXR_TOPIC_ID);
    const char* topic_xml = "<dds>"
            "<topic>"
            "<name>HelloWorldTopic</name>"
            "<dataType>HelloWorld</dataType>"
            "</topic>"
            "</dds>";
    uint16_t topic_req = uxr_buffer_create_topic_xml(&session, reliable_out, topic_id, participant_id, topic_xml,
                    UXR_REPLACE);
    // 创建publisher
    uxrObjectId publisher_id = uxr_object_id(0x01, UXR_PUBLISHER_ID);
    const char* publisher_xml = "";
    uint16_t publisher_req = uxr_buffer_create_publisher_xml(&session, reliable_out, publisher_id, participant_id,
                    publisher_xml, UXR_REPLACE);
    // 创建datawriter
    uxrObjectId datawriter_id = uxr_object_id(0x01, UXR_DATAWRITER_ID);
    const char* datawriter_xml = "<dds>"
            "<data_writer>"
            "<topic>"
            "<kind>NO_KEY</kind>"
            "<name>HelloWorldTopic</name>"
            "<dataType>HelloWorld</dataType>"
            "</topic>"
            "</data_writer>"
            "</dds>";
    uint16_t datawriter_req = uxr_buffer_create_datawriter_xml(&session, reliable_out, datawriter_id, publisher_id,
                    datawriter_xml, UXR_REPLACE);

    // Send create entities message and wait its status
    // 发送创建实体的message并等待状态
    uint8_t status[4];
    // 四个reguest组成一个数组
    uint16_t requests[4] = {
        participant_req, topic_req, publisher_req, datawriter_req
    };
    // 运行session直到所有的状态都返回
    if (!uxr_run_session_until_all_status(&session, 1000, requests, status, 4))
    {
        printf("Error at create entities: participant: %i topic: %i publisher: %i datawriter: %i\n", status[0],
                status[1], status[2], status[3]);
        return 1;
    }

    // Write topics
    // 写入话题
    bool connected = true;
    uint32_t count = 0;
    // 连接未超时并且 写入话题的计次不超过限定
    while (connected && count < max_topics)
    {
        HelloWorld topic = {
            ++count, "Hello DDS world!"
        };

        ucdrBuffer ub;
        uint32_t topic_size = HelloWorld_size_of_topic(&topic, 0);
        // 准备发送
        uxr_prepare_output_stream(&session, reliable_out, datawriter_id, &ub, topic_size);
        // 序列化topic
        HelloWorld_serialize_topic(&ub, &topic);

        printf("Send topic: %s, id: %i\n", topic.message, topic.index);
        // 确认连接，条件为所有流中的last_sent=last_ack
        // 循环执行发送output流中的消息，并监听来自agent的消息，直到等待一条新消息的时间超过了1000
        connected = uxr_run_session_time(&session, 1000);
    }

    // Delete resources
    uxr_delete_session(&session);
    uxr_close_udp_transport(&transport);

    return 0;
}
