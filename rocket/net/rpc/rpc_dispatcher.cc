#include <google/protobuf/service.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "rocket/net/rpc/rpc_dispatcher.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/common/log.h"
#include "rocket/common/error_code.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/rpc/rpc_closure.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_connection.h"
#include "rocket/common/run_time.h"

namespace rocket
{

#define DELETE_RESOURCE(XX) \
  if (XX != NULL)           \
  {                         \
    delete XX;              \
    XX = NULL;              \
  }

  static RpcDispatcher *g_rpc_dispatcher = NULL;

  RpcDispatcher *RpcDispatcher::GetRpcDispatcher()
  {
    if (g_rpc_dispatcher != NULL)
    {
      return g_rpc_dispatcher;
    }
    g_rpc_dispatcher = new RpcDispatcher;
    return g_rpc_dispatcher;
  }

  /// @brief 这个函数帮我们根据请求找到要执行的方法名
  /// @param request 
  /// @param response 
  /// @param connection 
  void RpcDispatcher::dispatch(AbstractProtocol::s_ptr request, AbstractProtocol::s_ptr response, TcpConnection *connection)
  {

    std::shared_ptr<TinyPBProtocol> req_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(request);
    std::shared_ptr<TinyPBProtocol> rsp_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(response);

    //获取方法全名
    std::string method_full_name = req_protocol->m_method_name;
    std::string service_name;
    std::string method_name;

    //获取message_id
    rsp_protocol->m_msg_id = req_protocol->m_msg_id;
    //给返回响应相应字段赋值
    rsp_protocol->m_method_name = req_protocol->m_method_name;

    //解析出服务名和方法名
    if (!parseServiceFullName(method_full_name, service_name, method_name))
    {
      setTinyPBError(rsp_protocol, ERROR_PARSE_SERVICE_NAME, "parse service name error");
      return;
    }

    //根据服务名找到对应服务表示
    auto it = m_service_map.find(service_name);
    if (it == m_service_map.end())
    {
      ERRORLOG("%s | sericve neame[%s] not found", req_protocol->m_msg_id.c_str(), service_name.c_str());
      setTinyPBError(rsp_protocol, ERROR_SERVICE_NOT_FOUND, "service not found");
      return;
    }

    service_s_ptr service = (*it).second;

    //通过service对象根据方法名字找出method服务
    const google::protobuf::MethodDescriptor *method = service->GetDescriptor()->FindMethodByName(method_name);
    if (method == NULL)
    {
      ERRORLOG("%s | method neame[%s] not found in service[%s]", req_protocol->m_msg_id.c_str(), method_name.c_str(), service_name.c_str());
      setTinyPBError(rsp_protocol, ERROR_SERVICE_NOT_FOUND, "method not found");
      return;
    }

    //通过method服务获得请求原型
    google::protobuf::Message *req_msg = service->GetRequestPrototype(method).New();

    // 反序列化，将 pb_data 反序列化为 req_msg
    if (!req_msg->ParseFromString(req_protocol->m_pb_data))
    {
      ERRORLOG("%s | deserilize error", req_protocol->m_msg_id.c_str(), method_name.c_str(), service_name.c_str());
      setTinyPBError(rsp_protocol, ERROR_FAILED_DESERIALIZE, "deserilize error");
      DELETE_RESOURCE(req_msg);
      return;
    }

    INFOLOG("%s | get rpc request[%s]", req_protocol->m_msg_id.c_str(), req_msg->ShortDebugString().c_str());

    //通过method服务获得响应原型
    google::protobuf::Message *rsp_msg = service->GetResponsePrototype(method).New();

    RpcController *rpc_controller = new RpcController();
    rpc_controller->SetLocalAddr(connection->getLocalAddr());
    rpc_controller->SetPeerAddr(connection->getPeerAddr());
    rpc_controller->SetMsgId(req_protocol->m_msg_id);

    RunTime::GetRunTime()->m_msgid = req_protocol->m_msg_id;
    RunTime::GetRunTime()->m_method_name = method_name;

    RpcClosure *closure = new RpcClosure(nullptr, [req_msg, rsp_msg, req_protocol, rsp_protocol, connection, rpc_controller, this]() mutable
                                         {
                                           if (!rsp_msg->SerializeToString(&(rsp_protocol->m_pb_data)))
                                           {
                                             ERRORLOG("%s | serilize error, origin message [%s]", req_protocol->m_msg_id.c_str(), rsp_msg->ShortDebugString().c_str());
                                             setTinyPBError(rsp_protocol, ERROR_FAILED_SERIALIZE, "serilize error");
                                           }
                                           // 分发成功
                                           else
                                           {
                                             rsp_protocol->m_err_code = 0;
                                             rsp_protocol->m_err_info = "";
                                             INFOLOG("%s | dispatch success, requesut[%s], response[%s]", req_protocol->m_msg_id.c_str(), req_msg->ShortDebugString().c_str(), rsp_msg->ShortDebugString().c_str());
                                           }

                                           std::vector<AbstractProtocol::s_ptr> replay_messages;
                                           replay_messages.emplace_back(rsp_protocol);
                                           connection->reply(replay_messages);
                                         });
    //调用方法
    service->CallMethod(method, rpc_controller, req_msg, rsp_msg, closure);
  }

  /// @brief 根据method_full_name解析出service_name和method_name，method_full_name的形式为service_name.method_name
  /// @param full_name 
  /// @param service_name 
  /// @param method_name 
  /// @return 
  bool RpcDispatcher::parseServiceFullName(const std::string &full_name, std::string &service_name, std::string &method_name)
  {
    if (full_name.empty())
    {
      ERRORLOG("full name empty");
      return false;
    }
    size_t i = full_name.find_first_of(".");
    if (i == full_name.npos)
    {
      ERRORLOG("not find . in full name [%s]", full_name.c_str());
      return false;
    }
    service_name = full_name.substr(0, i);
    method_name = full_name.substr(i + 1, full_name.length() - i - 1);

    INFOLOG("parse sericve_name[%s] and method_name[%s] from full name [%s]", service_name.c_str(), method_name.c_str(), full_name.c_str());

    return true;
  }

  /// @brief 将本地服务注册为RPC服务，也是给外部使用的
  /// @param service 
  void RpcDispatcher::registerService(service_s_ptr service)
  {
    std::string service_name = service->GetDescriptor()->full_name();
    m_service_map[service_name] = service;
  }

  /// @brief 设置错误信息
  /// @param msg 
  /// @param err_code 
  /// @param err_info 
  void RpcDispatcher::setTinyPBError(std::shared_ptr<TinyPBProtocol> msg, int32_t err_code, const std::string err_info)
  {
    msg->m_err_code = err_code;
    msg->m_err_info = err_info;
    msg->m_err_info_len = err_info.length();
  }

}