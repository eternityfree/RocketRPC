#include <unistd.h>
#include "rocket/common/log.h"
#include "rocket/net/fd_event_group.h"
#include "rocket/net/tcp/tcp_connection.h"
#include "rocket/net/coder/string_coder.h"
#include "rocket/net/coder/tinypb_coder.h"

namespace rocket
{

  TcpConnection::TcpConnection(EventLoop *event_loop, int fd, int buffer_size, NetAddr::s_ptr peer_addr, NetAddr::s_ptr local_addr, TcpConnectionType type /*= TcpConnectionByServer*/)
      : m_event_loop(event_loop), m_local_addr(local_addr), m_peer_addr(peer_addr), m_state(NotConnected), m_fd(fd), m_connection_type(type)
  {

    m_in_buffer = std::make_shared<TcpBuffer>(buffer_size);
    m_out_buffer = std::make_shared<TcpBuffer>(buffer_size);

    m_fd_event = FdEventGroup::GetFdEventGroup()->getFdEvent(fd);
    //设置非阻塞
    m_fd_event->setNonBlock();

    m_coder = new TinyPBCoder();

    if (m_connection_type == TcpConnectionByServer)
    {
      listenRead();
    }
  }

  TcpConnection::~TcpConnection()
  {
    DEBUGLOG("~TcpConnection");
    if (m_coder)
    {
      delete m_coder;
      m_coder = NULL;
    }
  }

  /// @brief 可读事件触发的回调函数，用于读取客户端发来的数据，组装成RPC请求
  /*服务端使用：解码RPC请求，调用业务逻辑接口，执行业务逻辑
  客户端使用：接收响应，解码响应
  */
  void TcpConnection::onRead()
  {
    // 1. 从 socket 缓冲区，调用 系统的 read 函数读取字节 in_buffer 里面

    if (m_state != Connected)
    {
      ERRORLOG("onRead error, client has already disconneced, addr[%s], clientfd[%d]", m_peer_addr->toString().c_str(), m_fd);
      return;
    }

    bool is_read_all = false;
    bool is_close = false;
    //读取读缓冲区的数据到in_buffer当中
    while (!is_read_all)
    {
      //buffer满了，扩容
      if (m_in_buffer->writeAble() == 0)
      {
        m_in_buffer->resizeBuffer(2 * m_in_buffer->m_buffer.size());
      }

      //in_buffer当中能读多少就从缓冲区读多少
      int read_count = m_in_buffer->writeAble();
      int write_index = m_in_buffer->writeIndex();

      int rt = read(m_fd, &(m_in_buffer->m_buffer[write_index]), read_count);
      DEBUGLOG("success read %d bytes from addr[%s], client fd[%d]", rt, m_peer_addr->toString().c_str(), m_fd);
      //rt表示读取的字节数
      if (rt > 0)
      {
        m_in_buffer->moveWriteIndex(rt);
        if (rt == read_count)
        {
          continue;
        }
        else if (rt < read_count)
        {
          is_read_all = true;
          break;
        }
      }
      //返回0表示对端已经关闭了
      else if (rt == 0)
      {
        is_close = true;
        break;
      }
      //设置非阻塞之后，读完了就会返回-1，同时errno的值会被置为EAGAIN
      else if (rt == -1 && errno == EAGAIN)
      {
        is_read_all = true;
        break;
      }
    }
     // 如果对端已经关闭，就清除
    if (is_close)
    {
      // TODO:
      INFOLOG("peer closed, peer addr [%s], clientfd [%d]", m_peer_addr->toString().c_str(), m_fd);
      clear();
      return;
    }

    if (!is_read_all)
    {
      ERRORLOG("not read all data");
    }

    // TODO: 简单的 echo, 后面补充 RPC 协议解析
    excute();
  }


  /// @brief 服务端使用：将RPC请求作为入参，调用业务逻辑得到RPC响应
  ///        客户端使用：解码RPC响应，执行回调
  void TcpConnection::excute()
  {
    //只对服务端生效
    if (m_connection_type == TcpConnectionByServer)
    {
      // 将 RPC 请求执行业务逻辑，获取 RPC 响应, 再把 RPC 响应发送回去
      std::vector<AbstractProtocol::s_ptr> result;
      m_coder->decode(result, m_in_buffer);
      for (size_t i = 0; i < result.size(); ++i)
      {
        // 1. 针对每一个请求，调用 rpc 方法，获取响应 message
        // 2. 将响应 message 放入到发送缓冲区，监听可写事件回包
        INFOLOG("success get request[%s] from client[%s]", result[i]->m_msg_id.c_str(), m_peer_addr->toString().c_str());

        std::shared_ptr<TinyPBProtocol> message = std::make_shared<TinyPBProtocol>();
        // message->m_pb_data = "hello. this is rocket rpc test data";
        // message->m_msg_id = result[i]->m_msg_id;

        RpcDispatcher::GetRpcDispatcher()->dispatch(result[i], message, this);
      }
    }
    else
    {
      // 从 buffer 里 decode 得到 message 对象, 执行其回调
      std::vector<AbstractProtocol::s_ptr> result;
      m_coder->decode(result, m_in_buffer);

      for (size_t i = 0; i < result.size(); ++i)
      {
        std::string msg_id = result[i]->m_msg_id;
        auto it = m_read_dones.find(msg_id);
        if (it != m_read_dones.end())
        {
          it->second(result[i]);
          m_read_dones.erase(it);
        }
      }
    }
  }

  void TcpConnection::reply(std::vector<AbstractProtocol::s_ptr> &replay_messages)
  {
    m_coder->encode(replay_messages, m_out_buffer);
    listenWrite();
  }


  /// @brief 写回调函数
  /*客户端使用：编码请求，发送RPC请求给服务端
  服务端使用：发送编码的RPC响应给客户端
  */
  void TcpConnection::onWrite()
  {
    // 将当前 out_buffer 里面的数据全部发送给 client

    if (m_state != Connected)
    {
      ERRORLOG("onWrite error, client has already disconneced, addr[%s], clientfd[%d]", m_peer_addr->toString().c_str(), m_fd);
      return;
    }

    //只有在client端才执行
    if (m_connection_type == TcpConnectionByClient)
    {
      //  1. 将 message encode 得到字节流
      // 2. 将字节流写入到 buffer 里面，然后全部发送

      std::vector<AbstractProtocol::s_ptr> messages;

      // 将多个还没编码的信息放入到message数组当中
      for (size_t i = 0; i < m_write_dones.size(); ++i) 
      {
        messages.push_back(m_write_dones[i].first);
      }

      //将信息编码之后写入到buffer当中
      m_coder->encode(messages, m_out_buffer);
    }

    //开始往对端发送buffer里面的数据
    bool is_write_all = false;
    while (true)
    {
      if (m_out_buffer->readAble() == 0)
      {
        DEBUGLOG("no data need to send to ip: [%s]", m_peer_addr->toString().c_str());
        is_write_all = true;
        break;
      }
      int write_size = m_out_buffer->readAble();
      int read_index = m_out_buffer->readIndex();

      int rt = write(m_fd, &(m_out_buffer->m_buffer[read_index]), write_size);

      if (rt >= write_size)
      {
        DEBUGLOG("no data need to send to client [%s]", m_peer_addr->toString().c_str());
        is_write_all = true;
        break;
      }
      if (rt == -1 && errno == EAGAIN)
      {
        // 发送缓冲区已满，不能再发送了。
        // 这种情况我们等下次 fd 可写的时候再次发送数据即可
        ERRORLOG("write data error, errno==EAGIN and rt == -1");
        break;
      }
    }
    // 写完了就要关闭监听套接字的写事件，防止重复触发
    if (is_write_all)
    {
      m_fd_event->cancle(FdEvent::OUT_EVENT);
      m_event_loop->addEpollEvent(m_fd_event);
    }

    //只有在client端才执行，这一步是消息成功发送后执行回调函数
    if (m_connection_type == TcpConnectionByClient)
    {
      for (size_t i = 0; i < m_write_dones.size(); ++i)
      {
        //取出回调函数，传入参数并执行
        m_write_dones[i].second(m_write_dones[i].first);
      }
      //清除
      m_write_dones.clear();
    }
  }

  /// @brief 设置连接状态
  /// @param state 
  void TcpConnection::setState(const TcpState state)
  {
    m_state = Connected;
  }

  TcpState TcpConnection::getState()
  {
    return m_state;
  }

  /// @brief 从epoll句柄上删除已经关闭的通信套接字
  void TcpConnection::clear()
  {
    // 处理一些关闭连接后的清理动作
    if (m_state == Closed)
    {
      return;
    }
    //设置对应套接字事件为删除
    m_fd_event->cancle(FdEvent::IN_EVENT);
    m_fd_event->cancle(FdEvent::OUT_EVENT);

    //从epoll句柄上删除
    m_event_loop->deleteEpollEvent(m_fd_event);

    //状态设置为关闭
    m_state = Closed;
  }

  /// @brief 服务器主动关闭连接
  void TcpConnection::shutdown()
  {
    if (m_state == Closed || m_state == NotConnected)
    {
      return;
    }

    // 处于半关闭
    m_state = HalfClosing;

    // 调用 shutdown 关闭读写，意味着服务器不会再对这个 fd 进行读写操作了
    // 发送 FIN 报文， 触发了四次挥手的第一个阶段
    // 当 fd 发生可读事件，但是可读的数据为0，即 对端发送了 FIN
    ::shutdown(m_fd, SHUT_RDWR);
  }

  void TcpConnection::setConnectionType(TcpConnectionType type)
  {
    m_connection_type = type;
  }

  void TcpConnection::listenWrite()
  {

    m_fd_event->listen(FdEvent::OUT_EVENT, std::bind(&TcpConnection::onWrite, this));
    m_event_loop->addEpollEvent(m_fd_event);
  }

  /// @brief 启动监听可读事件
  void TcpConnection::listenRead()
  {
    //绑定可读事件以及回调函数
    m_fd_event->listen(FdEvent::IN_EVENT, std::bind(&TcpConnection::onRead, this));
    //挂载到对应IO线程的epoll句柄上
    m_event_loop->addEpollEvent(m_fd_event);
  }

  /// @brief 将RPC请求以及回调写入connection当中的buffer
  /// @param message 
  /// @param done 
  void TcpConnection::pushSendMessage(AbstractProtocol::s_ptr message, std::function<void(AbstractProtocol::s_ptr)> done)
  {
    m_write_dones.push_back(std::make_pair(message, done));
  }

  void TcpConnection::pushReadMessage(const std::string &msg_id, std::function<void(AbstractProtocol::s_ptr)> done)
  {
    m_read_dones.insert(std::make_pair(msg_id, done));
  }

  NetAddr::s_ptr TcpConnection::getLocalAddr()
  {
    return m_local_addr;
  }

  NetAddr::s_ptr TcpConnection::getPeerAddr()
  {
    return m_peer_addr;
  }

  int TcpConnection::getFd()
  {
    return m_fd;
  }

}