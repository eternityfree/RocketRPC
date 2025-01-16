
#include "rocket/common/run_time.h"

namespace rocket
{

  thread_local RunTime *t_run_time = NULL;

  /// @brief 获取运行时对象
  /// @return 
  RunTime *RunTime::GetRunTime()
  {
    if (t_run_time)
    {
      return t_run_time;
    }
    t_run_time = new RunTime();
    return t_run_time;
  }

  RpcInterface *RunTime::getRpcInterface()
  {
    return m_rpc_interface;
  }

}
