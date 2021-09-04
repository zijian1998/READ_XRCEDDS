#include <uxr/client/profile/discovery/discovery.h>
#include <uxr/client/profile/transport/ip/ip.h>
#include <uxr/client/core/session/object_id.h>
#include <uxr/client/core/session/stream/seq_num.h>
#include <uxr/client/core/type/xrce_types.h>
#include <uxr/client/util/time.h>

#include "../../core/serialization/xrce_header_internal.h"
#include "../../core/session/submessage_internal.h"
#include "../../core/log/log_internal.h"
#include "transport/udp_transport_datagram_internal.h"

#include <string.h>

#define GET_INFO_MSG_SIZE   8
#define GET_INFO_REQUEST_ID 9

#define MULTICAST_DEFAULT_IP   "239.255.0.2"
#define MULTICAST_DEFAULT_PORT 7400

typedef struct CallbackData
{
    uxrOnAgentFound on_agent;
    void* args;

} CallbackData;

static void write_get_info_message(
        ucdrBuffer* ub);
static bool listen_info_message(
        uxrUDPTransportDatagram* transport,
        int period,
        CallbackData* callback);
static bool read_info_headers(
        ucdrBuffer* ub);
static bool read_info_message(
        ucdrBuffer* ub,
        CallbackData* callback);

//==================================================================
//                             PUBLIC
//==================================================================

void uxr_discovery_agents_default(
        uint32_t attempts,
        int period,
        uxrOnAgentFound on_agent_func,
        void* args)
{
    TransportLocator multicast;
    uxr_ip_to_locator(MULTICAST_DEFAULT_IP, (uint16_t)MULTICAST_DEFAULT_PORT, UXR_IPv4, &multicast);
    uxr_discovery_agents(attempts, period, on_agent_func, args, &multicast, 1);
}
/**
 * 发现agent
 * @ attemps:       （10）  尝试次数
 * @ period：       （1000）    周期
 * @ on_angt_func:  (on_agent_found)    函数
 * @ *args :        (null)      传参 无 是on_agent_found的参数
 * @ *agent_list:   (agent_list)    记录了agent的locator的列表
 * @ agent_list_size:(size)         agent的数量
 * */
void uxr_discovery_agents(
        uint32_t attempts,
        int period,
        uxrOnAgentFound on_agent_func,
        void* args,
        const TransportLocator* agent_list,
        size_t agent_list_size)
{
    CallbackData callback;          // CallbackData是一个结构体，包含一个函数指针，一个参数指针
    callback.on_agent = on_agent_func;  //赋值
    callback.args = args;               // 赋值

    uint8_t output_buffer[UXR_UDP_TRANSPORT_MTU_DATAGRAM];
    ucdrBuffer ub;      // 序列化buffer
    ucdr_init_buffer(&ub, output_buffer, UXR_UDP_TRANSPORT_MTU_DATAGRAM);   // 初始化ud
    // 执行uxr序列化操作
    write_get_info_message(&ub);
    // 获取buffer大小
    size_t message_length = ucdr_buffer_length(&ub);
    // UDP数据包
    uxrUDPTransportDatagram transport;
    // 初始化transport 如果成功则已经建立套结字，并加入到了套结字集
    if (uxr_init_udp_transport_datagram(&transport))    
    {
        bool is_agent_found = false;
        // 外层循环进行尝试
        for (uint32_t a = 0; a < attempts && !is_agent_found; ++a)
        {
            // 遍历agent_list
            for (size_t i = 0; i < agent_list_size; ++i)
            {
                // 发送序列化消息
                (void) uxr_udp_send_datagram_to(&transport, output_buffer, message_length, &agent_list[i]);
                UXR_DEBUG_PRINT_MESSAGE(UXR_SEND, output_buffer, message_length, 0);
            }
            // 发送完get_info后记录时间戳，以毫秒为单位
            int64_t timestamp = uxr_millis();
            // 剩余时限
            int poll = period;
            // 直到找到agent
            while (0 < poll && !is_agent_found)
            {
                // 监听info消息
                is_agent_found = listen_info_message(&transport, poll, &callback);
                // 剩余时限 = 当前时间 - 发送后的时间戳
                poll -= (int)(uxr_millis() - timestamp);
            }
        }
        // 关闭udp 套结字
        uxr_close_udp_transport_datagram(&transport);
    }
}

//==================================================================
//                             INTERNAL
//==================================================================

void write_get_info_message(
        ucdrBuffer* ub)
{
    // 载荷
    GET_INFO_Payload payload;
    // 设置标识请求的requestId为00
    payload.base.request_id = (RequestId){{
                                              0x00, GET_INFO_REQUEST_ID
                                          }
    };
    // 设置请求request目标，这里是agent(这里很奇怪，为什么是agent？)
    payload.base.object_id = DDS_XRCE_OBJECTID_AGENT;
    // XRCE Agent的活动信息
    payload.info_mask = INFO_CONFIGURATION | INFO_ACTIVITY;
    // 序列化message header 无clientkey的格式
    uxr_serialize_message_header(ub, SESSION_ID_WITHOUT_CLIENT_KEY, 0, 0, 0);
    // 序列化submessage header 为GET_INFO类型
    (void) uxr_buffer_submessage_header(ub, SUBMESSAGE_ID_GET_INFO, GET_INFO_MSG_SIZE, 0);
    // 序列化payload
    (void) uxr_serialize_GET_INFO_Payload(ub, &payload);
}
// 监听info信息
bool listen_info_message(
        uxrUDPTransportDatagram* transport,     // 套结字
        int poll,                               // 计数
        CallbackData* callback)                 // 回调函数
{
    // 
    uint8_t* input_buffer; size_t length;
    // 是否成功
    bool is_succeed = false;
    // 是否通过UDP Socket接受到消息，利用收到的消息进行填充buffer、赋值length
    bool received = uxr_udp_recv_datagram(transport, &input_buffer, &length, poll);
    // 如果接受到，则进行解析
    if (received)
    {
        UXR_DEBUG_PRINT_MESSAGE(UXR_RECV, input_buffer, length, 0);
        // 初始化buffer
        ucdrBuffer ub;
        ucdr_init_buffer(&ub, input_buffer, (uint32_t)length);
        // 从读到的buffer中获取header信息
        if (read_info_headers(&ub))
        {   // 从消息中获取是否成功获得agent
            is_succeed = read_info_message(&ub, callback);
        }
    }

    return is_succeed;
}
// 阅读header信息
bool read_info_headers(
        ucdrBuffer* ub)
{
    // 反序列化获得sessionid、steamid、sequenenum、client_key
    uint8_t session_id; uint8_t stream_id_raw; uxrSeqNum seq_num; uint8_t key[CLIENT_KEY_SIZE];
    uxr_deserialize_message_header(ub, &session_id, &stream_id_raw, &seq_num, key);
    // 反序列化从submessage获取submessage header信息，并返回
    uint8_t id; uint16_t length; uint8_t flags;
    return uxr_read_submessage_header(ub, &id, &length, &flags);
}
// 反序列化discoveery信息载荷
bool uxr_deserialize_discovery_INFO_Payload(
        ucdrBuffer* buffer,
        INFO_Payload* output)
{
    bool ret = true;
    // 反序列化基本对象回应，包括request和Status.
    ret &= uxr_deserialize_BaseObjectReply(buffer, &output->base);
    // 反序列化optional_config
    ret &= ucdr_deserialize_bool(buffer, &output->object_info.optional_config);
    // 如果反序列化的结果中optional_config为真
    if (output->object_info.optional_config == true)
    {   // 反序列化Objectariant，里面有各种信息
        ret &= uxr_deserialize_ObjectVariant(buffer, &output->object_info.config);
    }
    // 反序列化optional_activity
    ret &= ucdr_deserialize_bool(buffer, &output->object_info.optional_activity);
    // 如果optional_activity为真
    if (output->object_info.optional_activity == true)
    {
        // 反序列化种类
        ret &= ucdr_deserialize_uint8_t(buffer, &output->object_info.activity.kind);
        // 看是否是AGENT 对象
        ret &= output->object_info.activity.kind == DDS_XRCE_OBJK_AGENT;
        if (ret)
        {
            // 根据规范，如果是agent对象，则activity中包含了两个因素，
            // availability和address_seq(传输地址),并对此进行反序列化
            ret &= ucdr_deserialize_int16_t(buffer, &output->object_info.activity._.agent.availability);
            ret &= ucdr_deserialize_uint32_t(buffer, &output->object_info.activity._.agent.address_seq.size);

            // This function takes care of deserializing at least the possible address_seq items
            // if the sent sequence is too long for the allocated UXR_TRANSPORT_LOCATOR_SEQUENCE_MAX
            // 如果发送的Seq太长了，address_seq的大小太大了，大于了限定长度，就取限定长度。避免溢出
            output->object_info.activity._.agent.address_seq.size =
                    (output->object_info.activity._.agent.address_seq.size > UXR_TRANSPORT_LOCATOR_SEQUENCE_MAX)   ?
                    UXR_TRANSPORT_LOCATOR_SEQUENCE_MAX                                                         :
                    output->object_info.activity._.agent.address_seq.size;
            // 遍历address_seq,反序列化TransportLocator
            for (uint32_t i = 0; i < output->object_info.activity._.agent.address_seq.size && ret; i++)
            {
                ret &= uxr_deserialize_TransportLocator(buffer,
                                &output->object_info.activity._.agent.address_seq.data[i]);
            }
        }
    }
    return ret;
}
// 阅读info消息
bool read_info_message(
        ucdrBuffer* ub,
        CallbackData* callback)
{
    // 是否成功、载荷
    bool is_succeed = false;
    INFO_Payload payload;
    // 是否成功反序列化载荷,
    if (uxr_deserialize_discovery_INFO_Payload(ub, &payload))
    {
        // 获取Xrce的Version
        XrceVersion* version = &payload.object_info.config._.agent.xrce_version;
        // 获取XRCE Client可以访问、并接受create_client消息的agent的传输地址
        TransportLocatorSeq* locators = &payload.object_info.activity._.agent.address_seq;
        // 遍历地址
        for (size_t i = 0; i < (size_t)locators->size; ++i)
        {
            TransportLocator* transport = &locators->data[i];
            // 如果获取的消息中的xrce的ersion和自己的XRCE_VERSION完全相同，则可以进行回调。
            if (0 == memcmp(version->data, DDS_XRCE_XRCE_VERSION.data, sizeof(DDS_XRCE_XRCE_VERSION.data)))
            {
                // 调用回调函数，回调函数是一个返回值为bool类型，传参为const TransportLocator* locator, 
                // void* args的函数，一般由自己定义，这里调用的回调函数是打印ip地址
                is_succeed = callback->on_agent(transport, callback->args);
            }
        }
    }

    return is_succeed;
}
