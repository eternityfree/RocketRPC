#include <sys/time.h>
#include <sstream>
#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include "rocket/common/log.h"
#include "rocket/common/util.h"
#include "rocket/common/config.h"
#include "rocket/net/eventloop.h"
#include "rocket/common/run_time.h"

namespace rocket
{

  static Logger *g_logger = NULL;

  /// @brief 信号处理函数，用于在程序接收到特定信号时执行一些清理操作，并最终触发核心存储core dump
  /// @param signal_no 该参数表示接收到的信号类型，比如SIGSEGV,SIGABRT等
  void CoredumpHandler(int signal_no)
  {
    //记录错误日志
    ERRORLOG("progress received invalid signal, will exit");
    //将日志缓冲区中的所有内容刷新到日志文件或输出设备，确保在进程退出之前，所有日志信息都被正确记录。
    g_logger->flush();
    //等待线程结束
    pthread_join(g_logger->getAsyncLopger()->m_thread, NULL);
    pthread_join(g_logger->getAsyncAppLopger()->m_thread, NULL);

    //将信号处理方式恢复为默认行为。SIG_DFL 表示使用操作系统默认的信号处理行为，通常是终止进程。
    signal(signal_no, SIG_DFL);
    //触发默认行为（例如终止进程）
    raise(signal_no);
  }

/*****************************************同步日志******************************************************/

  /// @brief 读取logger变量，用于宏操作
  /// @return 
  Logger *Logger::GetGlobalLogger()
  {
    return g_logger;
  }

  /// @brief 构造函数，设置日志级别
  /// @param level 
  /// @param type 
  Logger::Logger(LogLevel level, int type /*=1*/) : m_set_level(level), m_type(type)
  {
    //如果日志级别是unknow，直接返回
    if (m_type == 0)
    {
      return;
    }
    //读取框架内部日志相关配置
    m_asnyc_logger = std::make_shared<AsyncLogger>(
        Config::GetGlobalConfig()->m_log_file_name + "_rpc",
        Config::GetGlobalConfig()->m_log_file_path,
        Config::GetGlobalConfig()->m_log_max_file_size);

    //读取外部使用的应用层日志相关配置
    m_asnyc_app_logger = std::make_shared<AsyncLogger>(
        Config::GetGlobalConfig()->m_log_file_name + "_app",
        Config::GetGlobalConfig()->m_log_file_path,
        Config::GetGlobalConfig()->m_log_max_file_size);
  }

  /// @brief 刷新到磁盘
  void Logger::flush()
  {
    syncLoop();
    m_asnyc_logger->stop();
    m_asnyc_logger->flush();

    m_asnyc_app_logger->stop();
    m_asnyc_app_logger->flush();
  }

  /// @brief 日志初始化函数，生成定时事件周期性从buffer当中取日志写入文件
  void Logger::init()
  {
    if (m_type == 0)
    {
      return;
    }
    //构建一个定时任务，周期执行，同时绑定回调
    m_timer_event = std::make_shared<TimerEvent>(Config::GetGlobalConfig()->m_log_sync_inteval, true, std::bind(&Logger::syncLoop, this));
    //将定时事件挂载到当前loop当中
    EventLoop::GetCurrentEventLoop()->addTimerEvent(m_timer_event);

    //一些错误处理
    signal(SIGSEGV, CoredumpHandler);
    signal(SIGABRT, CoredumpHandler);
    signal(SIGTERM, CoredumpHandler);
    signal(SIGKILL, CoredumpHandler);
    signal(SIGINT, CoredumpHandler);
    signal(SIGSTKFLT, CoredumpHandler);
  }
  
  /// @brief 定时事件回调
  void Logger::syncLoop()
  {
    //同步 m_buffer 到 async_logger 的buffer队尾
    std::vector<std::string> tmp_vec;
    ScopeMutex<Mutex> lock(m_mutex);
    tmp_vec.swap(m_buffer);
    lock.unlock();

    if (!tmp_vec.empty())
    {
      m_asnyc_logger->pushLogBuffer(tmp_vec);
    }
    tmp_vec.clear();

    // 同步 m_app_buffer 到 app_async_logger 的buffer队尾
    std::vector<std::string> tmp_vec2;
    ScopeMutex<Mutex> lock2(m_app_mutex);
    tmp_vec2.swap(m_app_buffer);
    lock2.unlock();

    if (!tmp_vec2.empty())
    {
      m_asnyc_app_logger->pushLogBuffer(tmp_vec2);
    }
  }

  /// @brief 初始化日志相关配置
  /// @param type 
  void Logger::InitGlobalLogger(int type /*=1*/)
  {

    LogLevel global_log_level = StringToLogLevel(Config::GetGlobalConfig()->m_log_level);
    printf("Init log level [%s]\n", LogLevelToString(global_log_level).c_str());
    g_logger = new Logger(global_log_level, type);
    //初始化
    g_logger->init();
  }

  /// @brief 枚举转字符串
  /// @param level 
  /// @return 
  std::string LogLevelToString(LogLevel level)
  {
    switch (level)
    {
    case Debug:
      return "DEBUG";

    case Info:
      return "INFO";

    case Error:
      return "ERROR";
    default:
      return "UNKNOWN";
    }
  }

  /// @brief 字符串转枚举
  /// @param log_level 
  /// @return 
  LogLevel StringToLogLevel(const std::string &log_level)
  {
    if (log_level == "DEBUG")
    {
      return Debug;
    }
    else if (log_level == "INFO")
    {
      return Info;
    }
    else if (log_level == "ERROR")
    {
      return Error;
    }
    else
    {
      return Unknown;
    }
  }

  /// @brief 将日志信息格式化成字符串  [INFO] [23-10-05 14:30:45.123] [1234:5678] [abc123] [processRequest]  模板：[日志级别] [时间戳] [进程ID:线程ID] [msgid] [method_name]
  /// @return 格式化后的字符串
  std::string LogEvent::toString()
  {
    //获取当前时间，并格式化为23-10-05 14:30:45.123这样的形式
    struct timeval now_time;

    gettimeofday(&now_time, nullptr);

    struct tm now_time_t;
    localtime_r(&(now_time.tv_sec), &now_time_t);

    char buf[128];
    strftime(&buf[0], 128, "%y-%m-%d %H:%M:%S", &now_time_t);
    std::string time_str(buf);
    int ms = now_time.tv_usec / 1000;
    time_str = time_str + "." + std::to_string(ms);

    //获取当前进程ID和线程ID
    m_pid = getPid();
    m_thread_id = getThreadId();

    std::stringstream ss;

    //拼接为字符串
    ss << "[" << LogLevelToString(m_level) << "]\t"
       << "[" << time_str << "]\t"
       << "[" << m_pid << ":" << m_thread_id << "]\t";

    // 获取当前线程处理的请求的 msgid
    std::string msgid = RunTime::GetRunTime()->m_msgid;
    std::string method_name = RunTime::GetRunTime()->m_method_name;

    //将当前消息ID以及方法名拼接到日志里面
    if (!msgid.empty())
    {
      ss << "[" << msgid << "]\t";
    }

    if (!method_name.empty())
    {
      ss << "[" << method_name << "]\t";
    }
    return ss.str();
  }

  void Logger::pushLog(const std::string &msg)
  {
    if (m_type == 0)
    {
      printf((msg + "\n").c_str());
      return;
    }
    ScopeMutex<Mutex> lock(m_mutex);
    m_buffer.push_back(msg);
    lock.unlock();
  }

  void Logger::pushAppLog(const std::string &msg)
  {
    ScopeMutex<Mutex> lock(m_app_mutex);
    m_app_buffer.push_back(msg);
    lock.unlock();
  }

  void Logger::log()
  {
  }



/*****************************************异步日志******************************************************/

  /// @brief 
  /// @param file_name  日志文件名称
  /// @param file_path  日志文件路径
  /// @param max_size   日志的最大容量
  AsyncLogger::AsyncLogger(const std::string &file_name, const std::string &file_path, int max_size)
      : m_file_name(file_name), m_file_path(file_path), m_max_file_size(max_size)
  {
    //初始化信号量
    sem_init(&m_sempahore, 0, 0);

    //创建一个线程执行loop回调
    assert(pthread_create(&m_thread, NULL, &AsyncLogger::Loop, this) == 0);

    // assert(pthread_cond_init(&m_condtion, NULL) == 0);
    //信号量的当前值为0，阻塞，直到loop线程执行到一定位置才解除阻塞,确保主线程在异步线程初始化完成后再继续执行，避免异步线程还在初始化就开始要求写日志
    sem_wait(&m_sempahore);
  }

  /// @brief loop循环，负责将buffer当中的数据写入到文件当中，写完了线程就休眠，它会检查当前日期，按日期分文件存储日志，并且在日志文件达到指定大小时切换文件
  /// @param arg 
  /// @return 
  void *AsyncLogger::Loop(void *arg)
  {
    // 将 buffer 里面的全部数据打印到文件中，然后线程睡眠，直到有新的数据再重复这个过程

    AsyncLogger *logger = reinterpret_cast<AsyncLogger *>(arg);

    //初始化条件变量
    assert(pthread_cond_init(&logger->m_condtion, NULL) == 0);

    //增加信号量的值，唤醒阻塞的线程，告诉主线程异步日志线程已经准备好了
    sem_post(&logger->m_sempahore);

    while (1)
    {
      //加锁
      ScopeMutex<Mutex> lock(logger->m_mutex);
      while (logger->m_buffer.empty())
      {
        //当日志缓冲区为空时，当前线程会进入等待状态，直到缓冲区中有日志需要写入
        pthread_cond_wait(&(logger->m_condtion), logger->m_mutex.getMutex()); 
      }


      //交换缓冲区buffer的数据到临时变量tmp，然后清空缓冲区，这是为了将缓冲区的数据批量写入文件
      std::vector<std::string> tmp;
      tmp.swap(logger->m_buffer.front());
      logger->m_buffer.pop();

      //解锁
      lock.unlock();

      //获取当前时间生成文件名
      timeval now;
      gettimeofday(&now, NULL);
      struct tm now_time;
      localtime_r(&(now.tv_sec), &now_time);
      //date数组当中存放的是以YYYYMMDD格式的本地日期
      const char *format = "%Y%m%d";
      char date[32];
      strftime(date, sizeof(date), format, &now_time);

      //如果日期发生变化，比如说进入新的一天，则重置文件编号，设置重新打开文件标志并更新日期
      if (std::string(date) != logger->m_date)
      {
        //日志文件序号置为0
        logger->m_no = 0;
        //打开标志置为true
        logger->m_reopen_flag = true;
        //更新日期
        logger->m_date = std::string(date);
      }

      //如果文件句柄为空，表示文件尚未打开，将重新打开标志位设为true，同时准备重新打开文件
      if (logger->m_file_hanlder == NULL)
      {
        logger->m_reopen_flag = true;
      }

      //构建日志的文件名，这比简单的用+号进行字符串拼接效率要好
      std::stringstream ss;
      ss << logger->m_file_path << logger->m_file_name << "_"
         << std::string(date) << "_log.";
      std::string log_file_name = ss.str() + std::to_string(logger->m_no);

      //如果当前文件需要重新打开，这说明当前日志文件是新的文件
      if (logger->m_reopen_flag)
      {
        //日志文件句柄不为空，关掉之前的旧文件
        if (logger->m_file_hanlder)
        {
          fclose(logger->m_file_hanlder);
        }
        //重新打开新文件，重新赋值日志文件句柄，同时将m_reopen_flag标志位设为false
        logger->m_file_hanlder = fopen(log_file_name.c_str(), "a");
        logger->m_reopen_flag = false;
      }

      //如果当前打开的文件里的字节大小大于允许的最大字节，关闭当前文件，重新打开新的一个文件，并将对应属性设置好
      if (ftell(logger->m_file_hanlder) > logger->m_max_file_size)
      {
        fclose(logger->m_file_hanlder);

        log_file_name = ss.str() + std::to_string(++logger->m_no);
        logger->m_file_hanlder = fopen(log_file_name.c_str(), "a");
        logger->m_reopen_flag = false;
      }

      //开始将日志写入文件当中
      for (auto &i : tmp)
      {
        if (!i.empty())
        {
          fwrite(i.c_str(), 1, i.length(), logger->m_file_hanlder);
        }
      }
      /*为什么需要fflush?
      * 在某些情况下，特别是在多个fwrite操作后，数据并不会立即写入文件，直到缓冲区满或者文件被关闭才会写入
      * 如果在程序崩溃、文件操作未完成或进程被中断的情况下，缓冲区的数据可能会被丢失。
      */
      //将缓冲区的数据立即写入到文件当中去
      fflush(logger->m_file_hanlder);
      //logger->flush();

      //有停止指令，立即退出
      if (logger->m_stop_flag)
      {
        return NULL;
      }
    }

    return NULL;
  }

  /// @brief 停止记录日志
  void AsyncLogger::stop()
  {
    m_stop_flag = true;
  }

  /// @brief 刷新缓冲区数据到日志文件
  void AsyncLogger::flush()
  {
    if (m_file_hanlder)
    {
      // 将输出缓冲区的数据立即刷新到文件当中，m_file_hanlder是当前打开文件的句柄
      fflush(m_file_hanlder);
    }
  }

  /// @brief 将日志写入buffer
  /// @param vec 
  void AsyncLogger::pushLogBuffer(std::vector<std::string> &vec)
  {
    ScopeMutex<Mutex> lock(m_mutex);
    //将日志插入队列
    m_buffer.push(vec);
    //唤醒因为buffer为空时阻塞的异步线程
    pthread_cond_signal(&m_condtion);

    lock.unlock();

    // 这时候需要唤醒异步日志线程
    // printf("pthread_cond_signal\n");
  }

}