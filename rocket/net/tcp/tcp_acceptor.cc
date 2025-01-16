#include <assert.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <string.h>
#include "rocket/common/log.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_acceptor.h"

namespace rocket
{
  /* 封装socket->bind->listen->accept的一个过程*/

  TcpAcceptor::TcpAcceptor(NetAddr::s_ptr local_addr) : m_local_addr(local_addr)
  {
    if (!local_addr->checkValid())
    {
      ERRORLOG("invalid local addr %s", local_addr->toString().c_str());
      exit(0);
    }

    m_family = m_local_addr->getFamily();

    //1.生成监听套接字
    m_listenfd = socket(m_family, SOCK_STREAM, 0);

    if (m_listenfd < 0)
    {
      ERRORLOG("invalid listenfd %d", m_listenfd);
      exit(0);
    }

    //2.设置端口复用,防止断开连接时端口还在time_wait状态，无法立即绑定相同端口
    int val = 1;
    if (setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) != 0)
    {
      ERRORLOG("setsockopt REUSEADDR error, errno=%d, error=%s", errno, strerror(errno));
    }

    //3.bind,绑定本地IP和端口
    socklen_t len = m_local_addr->getSockLen();
    if (bind(m_listenfd, m_local_addr->getSockAddr(), len) != 0)
    {
      ERRORLOG("bind error, errno=%d, error=%s", errno, strerror(errno));
      exit(0);
    }

    //4.listen设置监听
    if (listen(m_listenfd, 1000) != 0)
    {
      ERRORLOG("listen error, errno=%d, error=%s", errno, strerror(errno));
      exit(0);
    }
  }

  TcpAcceptor::~TcpAcceptor()
  {
  }

  /// @brief 获取用于监听的套接字
  /// @return 
  int TcpAcceptor::getListenFd()
  {
    return m_listenfd;
  }

  //5.accept等待客户端连接
  std::pair<int, NetAddr::s_ptr> TcpAcceptor::accept()
  {
    if (m_family == AF_INET)
    {
      sockaddr_in client_addr;
      memset(&client_addr, 0, sizeof(client_addr));
      socklen_t clien_addr_len = sizeof(client_addr);

      int client_fd = ::accept(m_listenfd, reinterpret_cast<sockaddr *>(&client_addr), &clien_addr_len);
      if (client_fd < 0)
      {
        ERRORLOG("accept error, errno=%d, error=%s", errno, strerror(errno));
      }
      //获取对端IP和port信息
      IPNetAddr::s_ptr peer_addr = std::make_shared<IPNetAddr>(client_addr);
      INFOLOG("A client have accpeted succ, peer addr [%s]", peer_addr->toString().c_str());

      return std::make_pair(client_fd, peer_addr);
    }
    //不是IPV4的话走这里，自己补充
    else
    {
      // ...
      return std::make_pair(-1, nullptr);
    }
  }

}