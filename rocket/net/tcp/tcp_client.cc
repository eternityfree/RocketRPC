#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include "rocket/common/log.h"
#include "rocket/net/tcp/tcp_client.h"
#include "rocket/net/eventloop.h"
#include "rocket/net/fd_event_group.h"
#include "rocket/common/error_code.h"
#include "rocket/net/tcp/net_addr.h"

namespace rocket
{

  TcpClient::TcpClient(NetAddr::s_ptr peer_addr) : m_peer_addr(peer_addr)
  {
    //获取loop对象
    m_event_loop = EventLoop::GetCurrentEventLoop();
    //创建client端的套接字
    m_fd = socket(peer_addr->getFamily(), SOCK_STREAM, 0);

    if (m_fd < 0)
    {
      ERRORLOG("TcpClient::TcpClient() error, failed to create fd");
      return;
    }

    //获取封装好的fd_event对象
    m_fd_event = FdEventGroup::GetFdEventGroup()->getFdEvent(m_fd);
    //设置非阻塞
    m_fd_event->setNonBlock();
    //获取tcp connection对象
    m_connection = std::make_shared<TcpConnection>(m_event_loop, m_fd, 128, peer_addr, nullptr, TcpConnectionByClient);
    //设置tcp connection连接属性为client端发起的连接
    m_connection->setConnectionType(TcpConnectionByClient);
  }

  TcpClient::~TcpClient()
  {
    DEBUGLOG("TcpClient::~TcpClient()");
    if (m_fd > 0)
    {
      close(m_fd);
    }
  }

  // 异步的进行 connect
  // 如果connect 成功，done 会被执行
  void TcpClient::connect(std::function<void()> done)
  {
    //建立连接，有下面两张情况
    /*
    1.直接返回0，表示建立连接成功
    2.返回 -1，但 errno== EINPROGRESS，表示连接正在建立，此时可以添加到 epoll 中去监听其可写事件。
    等待可写事件就绪后，调用 getsockopt 获取fd上的错误，错误为0代表连接建立成功。
    3.返回其他值，直接报错
    */

    int rt = ::connect(m_fd, m_peer_addr->getSockAddr(), m_peer_addr->getSockLen());
    //1.返回0，连接成功，设置连接状态为成功
    if (rt == 0)
    {
      DEBUGLOG("connect [%s] sussess", m_peer_addr->toString().c_str());
      m_connection->setState(Connected);
      initLocalAddr();
      //回调函数不为空，直接执行回调函数
      if (done)
      {
        done();
      }
    }
    //2.返回值为-1
    else if (rt == -1)
    {
      //错误码是EINPROGRESS，表示正在连接过程中
      if (errno == EINPROGRESS)
      {
        // epoll 监听可写事件，然后判断错误码，为什么要监听可写事件？因为当连接完成时，文件描述符可写，从而触发该事件，执行回调函数中的逻辑。
        m_fd_event->listen(FdEvent::OUT_EVENT,
                           [this, done]()
                           {
                            //尝试重新连接
                             int rt = ::connect(m_fd, m_peer_addr->getSockAddr(), m_peer_addr->getSockLen());
                             //rt < 0 && errno == EISCONN：表示连接已经建立（即连接操作已完成并且成功），但返回值为负数时，errno 会被设置为 EISCONN。
                             if ((rt < 0 && errno == EISCONN) || (rt == 0))
                             {
                               DEBUGLOG("connect [%s] sussess", m_peer_addr->toString().c_str());
                               initLocalAddr();
                               m_connection->setState(Connected);
                             }
                             //连接失败
                             else
                             {
                              //说明连接被拒绝
                               if (errno == ECONNREFUSED)
                               {
                                 m_connect_error_code = ERROR_PEER_CLOSED;
                                 m_connect_error_info = "connect refused, sys error = " + std::string(strerror(errno));
                               }
                               //连接失败，未知错误
                               else
                               {
                                 m_connect_error_code = ERROR_FAILED_CONNECT;
                                 m_connect_error_info = "connect unkonwn error, sys error = " + std::string(strerror(errno));
                               }
                               ERRORLOG("connect errror, errno=%d, error=%s", errno, strerror(errno));
                               //关闭当前套接字，并重新创建一个套接字
                               close(m_fd);
                               m_fd = socket(m_peer_addr->getFamily(), SOCK_STREAM, 0);
                             }

                             // 连接完后需要去掉可写事件的监听，不然会一直触发
                             m_event_loop->deleteEpollEvent(m_fd_event);
                             DEBUGLOG("now begin to done");
                             // 如果连接完成，才会执行回调函数
                             if (done)
                             {
                               done();
                             }
                           });
        
        //设置监听
        m_event_loop->addEpollEvent(m_fd_event);

        if (!m_event_loop->isLooping())
        {
          m_event_loop->loop();
        }
      }
      //3.返回其它，直接报错
      else
      {
        ERRORLOG("connect errror, errno=%d, error=%s", errno, strerror(errno));
        m_connect_error_code = ERROR_FAILED_CONNECT;
        m_connect_error_info = "connect error, sys error = " + std::string(strerror(errno));
        if (done)
        {
          done();
        }
      }
    }
  }

  /// @brief 停止loop
  void TcpClient::stop()
  {
    if (m_event_loop->isLooping())
    {
      m_event_loop->stop();
    }
  }

  // 异步的发送 message
  // 如果发送 message 成功，会调用 done 函数， 函数的入参就是 message 对象
  void TcpClient::writeMessage(AbstractProtocol::s_ptr message, std::function<void(AbstractProtocol::s_ptr)> done)
  {
    // 1. 把 message 对象写入到 Connection 的 buffer, done 也写入
    // 2. 启动 connection 可写事件
    m_connection->pushSendMessage(message, done);
    m_connection->listenWrite();
  }

  // 异步的读取 message
  // 如果读取 message 成功，会调用 done 函数， 函数的入参就是 message 对象
  void TcpClient::readMessage(const std::string &msg_id, std::function<void(AbstractProtocol::s_ptr)> done)
  {
    // 1. 监听可读事件
    // 2. 从 buffer 里 decode 得到 message 对象, 判断是否 msg_id 相等，相等则读成功，执行其回调
    m_connection->pushReadMessage(msg_id, done);
    m_connection->listenRead();
  }

  /// @brief 获取错误码
  /// @return 
  int TcpClient::getConnectErrorCode()
  {
    return m_connect_error_code;
  }

  /// @brief 获取错误信息
  /// @return 
  std::string TcpClient::getConnectErrorInfo()
  {
    return m_connect_error_info;
  }

  /// @brief 获取服务端地址
  /// @return 
  NetAddr::s_ptr TcpClient::getPeerAddr()
  {
    return m_peer_addr;
  }

  /// @brief 获取本机地址
  /// @return 
  NetAddr::s_ptr TcpClient::getLocalAddr()
  {
    return m_local_addr;
  }

  /// @brief 初始化本地地址
  void TcpClient::initLocalAddr()
  {
    sockaddr_in local_addr;
    socklen_t len = sizeof(local_addr);

    //getsockname关键字返回与套接字关联的本地地址（IP 地址和端口），储存在local_addr变量当中
    int ret = getsockname(m_fd, reinterpret_cast<sockaddr *>(&local_addr), &len);
    if (ret != 0)
    {
      ERRORLOG("initLocalAddr error, getsockname error. errno=%d, error=%s", errno, strerror(errno));
      return;
    }

    // 保存起来
    m_local_addr = std::make_shared<IPNetAddr>(local_addr);
  }

  /// @brief 添加定时事件
  /// @param timer_event 
  void TcpClient::addTimerEvent(TimerEvent::s_ptr timer_event)
  {
    m_event_loop->addTimerEvent(timer_event);
  }

}