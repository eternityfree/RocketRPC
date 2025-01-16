#include "rocket/net/tcp/tcp_server.h"
#include "rocket/net/eventloop.h"
#include "rocket/net/tcp/tcp_connection.h"
#include "rocket/common/log.h"
#include "rocket/common/config.h"

namespace rocket
{

  TcpServer::TcpServer(NetAddr::s_ptr local_addr) : m_local_addr(local_addr)
  {

    init();

    INFOLOG("rocket TcpServer listen sucess on [%s]", m_local_addr->toString().c_str());
  }

  TcpServer::~TcpServer()
  {
    if (m_main_event_loop)
    {
      delete m_main_event_loop;
      m_main_event_loop = NULL;
    }
    if (m_io_thread_group)
    {
      delete m_io_thread_group;
      m_io_thread_group = NULL;
    }
    if (m_listen_fd_event)
    {
      delete m_listen_fd_event;
      m_listen_fd_event = NULL;
    }
  }

  void TcpServer::init()
  {

    m_acceptor = std::make_shared<TcpAcceptor>(m_local_addr);

    //构造一个主reactor对象
    m_main_event_loop = EventLoop::GetCurrentEventLoop();
    //构造IOThreadGroup对象，里面封装了指定大小个subReactor
    m_io_thread_group = new IOThreadGroup(Config::GetGlobalConfig()->m_io_threads);

    //根据监听套接字构造一个封装好的fd_event
    m_listen_fd_event = new FdEvent(m_acceptor->getListenFd());
    //绑定读事件以及回调函数
    m_listen_fd_event->listen(FdEvent::IN_EVENT, std::bind(&TcpServer::onAccept, this));
    //挂载到epoll句柄上，执行这段代码肯定是主线程，自然而然就是挂载到主线程的epoll句柄上，这正好印证了主线程负责监听新用户连接
    m_main_event_loop->addEpollEvent(m_listen_fd_event);

    //设置定时任务，回调函数是ClearClientTimerFunc，这个函数用来定时清除已经关闭的连接
    m_clear_client_timer_event = std::make_shared<TimerEvent>(5000, true, std::bind(&TcpServer::ClearClientTimerFunc, this));
    m_main_event_loop->addTimerEvent(m_clear_client_timer_event);
  }

  void TcpServer::onAccept()
  {
    //主动调用封装好的accept函数，获得通信套接字client_fd,以及对端信息{元组形式}
    auto re = m_acceptor->accept();
    int client_fd = re.first;
    NetAddr::s_ptr peer_addr = re.second;

    //记录连接的用户数
    m_client_counts++;

    // 把 clientfd 添加到任意 IO 线程里面，即在主线程当中有新用户连接时，就会将生成的通信套接字分发给任意subReactor当中去
    IOThread *io_thread = m_io_thread_group->getIOThread();
    TcpConnection::s_ptr connetion = std::make_shared<TcpConnection>(io_thread->getEventLoop(), client_fd, 128, peer_addr, m_local_addr);
    //设置状态为已连接
    connetion->setState(Connected);

    //将当前连接放入连接集合当中去
    m_client.insert(connetion);

    INFOLOG("TcpServer succ get client, fd=%d", client_fd);
  }


  /// @brief 对外接口，启动函数
  void TcpServer::start()
  {
    m_io_thread_group->start();
    m_main_event_loop->loop();
  }

  /// @brief 定时任务的回调函数，定期清理已关闭连接的客户端
  void TcpServer::ClearClientTimerFunc()
  {
    auto it = m_client.begin();
    for (it = m_client.begin(); it != m_client.end();)
    {
      // TcpConnection::ptr s_conn = i.second;
      // DebugLog << "state = " << s_conn->getState();
      if ((*it) != nullptr && (*it).use_count() > 0 && (*it)->getState() == Closed)
      {
        // need to delete TcpConnection
        DEBUGLOG("TcpConection [fd:%d] will delete, state=%d", (*it)->getFd(), (*it)->getState());
        it = m_client.erase(it);
      }
      else
      {
        it++;
      }
    }
  }

}