// Copyright 2021-present Proyectos y Sistemas de Mantenimiento SL (eProsima).
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

#include <uxr/agent/transport/custom/CustomAgent.hpp>
#include <uxr/agent/transport/endpoint/IPv4EndPoint.hpp>

#include <poll.h>           // 多路复用
#include <sys/socket.h>     // 套接字相关
#include <unistd.h>         // 
#include <signal.h>         // 信号量相关
#include <netinet/in.h>     // 网络编程
#include <arpa/inet.h>      // 网络编程

/**
 * This custom XRCE Agent example attempts to show how easy is for the user to define a custom
 * Micro XRCE-DDS Agent behaviour, in terms of transport initialization and closing, and also
 * regarding read and write operations.
 * For this simple case, an UDP socket is opened on port 8888. Additionally, some information
 * messages are being printed to demonstrate the custom behaviour.
 * As the endpoint is already defined, we are using the provided
 * `eprosima::uxr::IPv4EndPoint` by the library.
 * Other transport protocols might need to implement their own endpoint struct.
 */

int main(int argc, char** argv)
{
    // 设置中间件种类
    eprosima::uxr::Middleware::Kind mw_kind(eprosima::uxr::Middleware::Kind::FASTDDS);
    // 设置agent的端口号为8888
    uint16_t agent_port(8888);
    // 定义pollfd指针  是用来IO多路复用的
    struct poll  fd poll_fd;

    /**
     * @brief Agent's initialization behaviour description.
     * lambda表达式，Agnet的初始化动作
     */
    eprosima::uxr::CustomAgent::InitFunction init_function = [&]() -> bool
    {
        bool rv = false;                                // 返回为false
        poll_fd.fd = socket(PF_INET, SOCK_DGRAM, 0);    // 协议族为IPv4协议族，类型为数据报套结字

        if (-1 != poll_fd.fd)   //不为负数说明成功
        {
            struct sockaddr_in address{};   // 处理网络通信的地址

            address.sin_family = AF_INET;           // 地址族设置为IPv4协议族
            address.sin_port = htons(agent_port);   // agent的TCP/UDP端口绑定到socket
            address.sin_addr.s_addr = INADDR_ANY;   // 服务器进程可以在任意网络接口（IP地址）接受客户连接
            memset(address.sin_zero, '\0', sizeof(address.sin_zero));   // 不使用
            // bind函数，把本地协议地址（IP地址+端口号）赋予套结字 不为负数则成功
            if (-1 != bind(poll_fd.fd,
                           reinterpret_cast<struct sockaddr*>(&address),    // 将address强转为sockaddr类型
                           sizeof(address)))
            {
                poll_fd.events = POLLIN;    // 识别事件为普通（TCP、UDP数据、读半部关闭数据FIN等）或优先级带数据（TCP的带外数据）可读
                rv = true;      // 返回值设置为true 表示已经bind

                UXR_AGENT_LOG_INFO(
                    UXR_DECORATE_GREEN(
                        "This is an example of a custom Micro XRCE-DDS Agent INIT function"),
                    "port: {}",
                    agent_port);
            }
        }

        return rv;
    };

    /**
     * @brief Agent's destruction actions.
     * Agent的销毁动作
     */
    eprosima::uxr::CustomAgent::FiniFunction fini_function = [&]() -> bool
    {
        // 如果poll_fd的套结字描述符==-1 可能因为socket函数没调用成功，不需要销毁，直接返回true
        if (-1 == poll_fd.fd)
        {
            return true;
        }
        // 根据套结字描述符执行close操作  ::表示局部的close 自己封装的
        if (0 == ::close(poll_fd.fd))
        {
            poll_fd.fd = -1;    // 设置socket套结字为-1

            UXR_AGENT_LOG_INFO(
                UXR_DECORATE_GREEN(
                    "This is an example of a custom Micro XRCE-DDS Agent FINI function"),
                "port: {}",
                agent_port);

            return true; // 返回true
        }
        else
        {
            return false;   // 返回false
        }
    };

    /**
     * @brief Agent's incoming data functionality.
     * Agent的传入数据功能
     */
    eprosima::uxr::CustomAgent::RecvMsgFunction recv_msg_function = [&](
            eprosima::uxr::CustomEndPoint* source_endpoint,     // 源端点
            uint8_t* buffer,                                    // buffer
            size_t buffer_length,                               // buffer的长度
            int timeout,                                        // 超时时长
            eprosima::uxr::TransportRc& transport_rc) -> ssize_t 
    {
        struct sockaddr_in client_addr{};   // 客户端的地址
        socklen_t client_addr_len = sizeof(struct sockaddr_in); // 客户端地址长度
        ssize_t bytes_received = -1;        // 接收到的字节数

        int poll_rv = poll(&poll_fd, 1, timeout);   // 得到poll的返回值
        // 返回值大于0，说明有描述符就绪了。
        if (0 < poll_rv) 
        {
            bytes_received = recvfrom(
                poll_fd.fd,
                buffer,
                buffer_length,
                0,
                reinterpret_cast<struct sockaddr *>(&client_addr),
                &client_addr_len);  // 从内核拷贝发过来的数据
            // 如果拷贝到了一些数据 返回ok，否则返回错误
            transport_rc = (-1 != bytes_received)   
                ? eprosima::uxr::TransportRc::ok    // 返回ok
                : eprosima::uxr::TransportRc::server_error; // 
        }
        else
        {   // 如果没有任何描述符就绪，返回超时错误，否则返回服务器错误
            transport_rc = (0 == poll_rv)
                ? eprosima::uxr::TransportRc::timeout_error
                : eprosima::uxr::TransportRc::server_error;
            bytes_received = 0; // 接受字节改为0
        }
        // 如果成功拷贝到了数据
        if (eprosima::uxr::TransportRc::ok == transport_rc)
        {
            UXR_AGENT_LOG_INFO(
                UXR_DECORATE_GREEN(
                    "This is an example of a custom Micro XRCE-DDS Agent RECV_MSG function"),
                "port: {}",
                agent_port);
            // 将源端点（client）的IP和端口号进行设置。
            source_endpoint->set_member_value<uint32_t>("address",
                static_cast<uint32_t>(client_addr.sin_addr.s_addr));
            source_endpoint->set_member_value<uint16_t>("port",
                static_cast<uint16_t>(client_addr.sin_port));
        }


        return bytes_received; // 返回接收到的字节数
    };

    /**
     * @brief Agent's outcoming data flow definition.
     * Agent输出数据流
     */
    eprosima::uxr::CustomAgent::SendMsgFunction send_msg_function = [&](
        const eprosima::uxr::CustomEndPoint* destination_endpoint,      // 目标端点
        uint8_t* buffer,                                                // buffer
        size_t message_length,                                          // 消息长度
        eprosima::uxr::TransportRc& transport_rc) -> ssize_t
    {
        struct sockaddr_in client_addr{};   // 客户端地址

        memset(&client_addr, 0, sizeof(client_addr));   // 填为0
        client_addr.sin_family = AF_INET;               // 地址族为IPv4
        client_addr.sin_port = destination_endpoint->get_member<uint16_t>("port");  // 设置端口
        client_addr.sin_addr.s_addr = destination_endpoint->get_member<uint32_t>("address");    // 设置地址

        ssize_t bytes_sent =
            sendto(
                poll_fd.fd,
                buffer,
                message_length,
                0,
                reinterpret_cast<struct sockaddr*>(&client_addr),
                sizeof(client_addr));   // 发送数据

        transport_rc = (-1 != bytes_sent)
            ? eprosima::uxr::TransportRc::ok
            : eprosima::uxr::TransportRc::server_error;

        if (eprosima::uxr::TransportRc::ok == transport_rc)
        {
            UXR_AGENT_LOG_INFO(
                UXR_DECORATE_GREEN(
                    "This is an example of a custom Micro XRCE-DDS Agent SEND_MSG function"),
                "port: {}",
                agent_port);
        }

        return bytes_sent;
    };

    /**
     * Run the main application.
     */
    try
    {
        /**
         * EndPoint definition for this transport. We define an address and a port.
         */
        eprosima::uxr::CustomEndPoint custom_endpoint;
        custom_endpoint.add_member<uint32_t>("address");
        custom_endpoint.add_member<uint16_t>("port");

        /**
         * Create a custom agent instance.
         * 创建一个agent实例
         */
        eprosima::uxr::CustomAgent custom_agent(
            "UDPv4_CUSTOM",     // 由这个customagent建立的中间件的名字
            &custom_endpoint,   // 
            mw_kind,            // DDS世界中custom agent的实体
            false,
            init_function,      // 初始化函数
            fini_function,      // 销毁函数
            send_msg_function,  // 发送消息函数
            recv_msg_function); // 接受消息函数

        /**
         * Set verbosity level
         */
        custom_agent.set_verbose_level(6);  // 设置log冗余级别

        /**
         * Run agent and wait until receiving an stop signal.
         */
        custom_agent.start();   // 开始运行

        int n_signal = 0;
        sigset_t signals;
        sigwait(&signals, &n_signal);

        /**
         * Stop agent, and exit.
         */
        custom_agent.stop();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cout << e.what() << std::endl;
        return 1;
    }
}