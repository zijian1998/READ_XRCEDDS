// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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

#ifndef UXR_AGENT_CLIENT_PROXYCLIENT_HPP_
#define UXR_AGENT_CLIENT_PROXYCLIENT_HPP_

#include <uxr/agent/middleware/Middleware.hpp>
#include <uxr/agent/participant/Participant.hpp>
#include <uxr/agent/client/session/Session.hpp>
#include <unordered_map>
#include <array>

namespace eprosima {
namespace uxr {
// std::enable_shared_from_this作用：当ProxyCient类型对象被一个智能指针对象管理使，
// 调用shared_from_this函数可以返回一个新的智能指针对象，新的指针对象也可以管理ProxyClient对象
class ProxyClient : public std::enable_shared_from_this<ProxyClient>
{
public:
    // enum class： alive和dead属于State的作用域
    enum class State : uint8_t
    {
        alive,
        dead
    };
    // explict表示显示构造函数 防止类构造函数的隐式自动转换 这里主要防止representation的隐式自动转换
    explicit ProxyClient(
            const dds::xrce::CLIENT_Representation& representation,
            Middleware::Kind middleware_kind = Middleware::Kind(0),     // 中间件种类 enum class 初始化为0
            std::unordered_map<std::string, std::string>&& properties = {});    // 右值引用
    // 析构函数 允许编译器默认合成的析构函数
    ~ProxyClient() = default;
	// 禁止使用编译器默认合成的函数版本
    ProxyClient(ProxyClient&&) = delete;
    ProxyClient(const ProxyClient&) = delete;
    ProxyClient& operator=(ProxyClient&&) = delete;
    ProxyClient& operator=(const ProxyClient&) = delete;
	// 创建对象
    dds::xrce::ResultStatus create_object(
            const dds::xrce::CreationMode& creation_mode,
            const dds::xrce::ObjectPrefix& objectid_prefix,
            const dds::xrce::ObjectVariant& object_representation);
	// 删除对象
    dds::xrce::ResultStatus delete_object(
            const dds::xrce::ObjectId& object_id);
	// 更新对象
    dds::xrce::ResultStatus update(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::ObjectVariant& representation);
	// 获取消息
    dds::xrce::ObjectInfo get_info(const dds::xrce::ObjectId& object_id);
	// 获取对象，返回管理对象的智能指针
    std::shared_ptr<XRCEObject> get_object(const dds::xrce::ObjectId& object_id);
	// 获取client_key 返回常量引用 第二个const表示该函数内不能改变类成员变量
    const dds::xrce::ClientKey& get_client_key() const { return representation_.client_key(); }
	// 获取session的id
    dds::xrce::SessionId get_session_id() const { return representation_.session_id(); }

    void release();

    Session& session();

    State get_state();

    void update_state();

    Middleware& get_middleware() { return *middleware_ ; };

private:
    bool create_object(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::ObjectVariant& representation,
            dds::xrce::ResultStatus& result_status);

    bool create_participant(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::OBJK_PARTICIPANT_Representation& representation,
            dds::xrce::ResultStatus& result_status);

    bool create_topic(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::OBJK_TOPIC_Representation& representation,
            dds::xrce::ResultStatus& result_status);

    bool create_publisher(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::OBJK_PUBLISHER_Representation& representation,
            dds::xrce::ResultStatus& result_status);

    bool create_subscriber(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::OBJK_SUBSCRIBER_Representation& representation,
            dds::xrce::ResultStatus& result_status);

    bool create_datawriter(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::DATAWRITER_Representation& representation,
            dds::xrce::ResultStatus& result_status);

    bool create_datareader(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::DATAREADER_Representation& representation,
            dds::xrce::ResultStatus& result_status);

    bool create_requester(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::REQUESTER_Representation& representation,
            dds::xrce::ResultStatus& result_status);

    bool create_replier(
            const dds::xrce::ObjectId& object_id,
            const dds::xrce::REPLIER_Representation& representation,
            dds::xrce::ResultStatus& result_status);

    bool delete_object_unlock(
            const dds::xrce::ObjectId& object_id);

private:
    const dds::xrce::CLIENT_Representation representation_;		// client的表示代表
    std::unique_ptr<Middleware> middleware_;					// 采用的中间件，使用unique_ptr进行管理 也就是智能指针独享被管理对象
    std::mutex mtx_;											// 普通互斥锁
    XRCEObject::ObjectContainer objects_;						// 一个hash表，key是object_id，value是管理object对象的智能指针
    Session session_;											// session
    std::mutex state_mtx_;										// 状态的普通互斥锁
    State state_;												// 状态
    std::chrono::time_point<std::chrono::steady_clock> timestamp_;	// 时间戳
    std::unordered_map<std::string, std::string> properties_;	// 性质 
};

} // namespace uxr
} // namespace eprosima

#endif // UXR_AGENT_CLIENT_PROXYCLIENT_HPP_
