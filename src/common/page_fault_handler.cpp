#include "page_fault_handler.h"
#include "common/log.h"
#include <algorithm>
#include <cstring>
#include <mutex>
Log_SetChannel(Common::PageFaultHandler);

#if defined(WIN32)
#include "common/windows_headers.h"
#elif defined(__linux__) || defined(__ANDROID__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#define USE_SIGSEGV 1
#endif

namespace Common::PageFaultHandler {

struct RegisteredHandler
{
  void* owner;
  Callback callback;
};
static std::vector<RegisteredHandler> m_handlers;
static std::mutex m_handler_lock;
static thread_local bool s_in_handler;

#if defined(WIN32)
static PVOID s_veh_handle;

static LONG ExceptionHandler(PEXCEPTION_POINTERS exi)
{
  if (exi->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || s_in_handler)
    return EXCEPTION_CONTINUE_SEARCH;

  s_in_handler = true;

  void* const exception_pc = reinterpret_cast<void*>(exi->ContextRecord->Rip);
  void* const exception_address = reinterpret_cast<void*>(exi->ExceptionRecord->ExceptionInformation[1]);
  bool const is_write = exi->ExceptionRecord->ExceptionInformation[0] == 1;

  std::lock_guard<std::mutex> guard(m_handler_lock);
  for (const RegisteredHandler& rh : m_handlers)
  {
    if (rh.callback(exception_pc, exception_address, is_write) == HandlerResult::ContinueExecution)
    {
      s_in_handler = false;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }

  s_in_handler = false;
  return EXCEPTION_CONTINUE_SEARCH;
}

#elif defined(USE_SIGSEGV)

static struct sigaction s_old_sigsegv_action;

static void SIGSEGVHandler(int sig, siginfo_t* info, void* ctx)
{
  if ((info->si_code != SEGV_MAPERR && info->si_code != SEGV_ACCERR) || s_in_handler)
    return;

  void* const exception_address = reinterpret_cast<void*>(info->si_addr);

#if defined(__x86_64__)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(__aarch64__)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.pc);
  const bool is_write = false;
#else
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

  std::lock_guard<std::mutex> guard(m_handler_lock);
  for (const RegisteredHandler& rh : m_handlers)
  {
    if (rh.callback(exception_pc, exception_address, is_write) == HandlerResult::ContinueExecution)
    {
      s_in_handler = false;
      return;
    }
  }

  // call old signal handler
  if (s_old_sigsegv_action.sa_flags & SA_SIGINFO)
    s_old_sigsegv_action.sa_sigaction(sig, info, ctx);
  else if (s_old_sigsegv_action.sa_handler == SIG_DFL)
    signal(sig, SIG_DFL);
  else if (s_old_sigsegv_action.sa_handler == SIG_IGN)
    return;
  else
    s_old_sigsegv_action.sa_handler(sig);
}

#endif

bool InstallHandler(void* owner, Callback callback)
{
  bool was_empty;
  {
    std::lock_guard<std::mutex> guard(m_handler_lock);
    if (std::find_if(m_handlers.begin(), m_handlers.end(),
                     [owner](const RegisteredHandler& rh) { return rh.owner == owner; }) != m_handlers.end())
    {
      return false;
    }

    was_empty = m_handlers.empty();
    m_handlers.push_back(RegisteredHandler{owner, std::move(callback)});
  }

  if (was_empty)
  {
#if defined(WIN32)
    s_veh_handle = AddVectoredExceptionHandler(1, ExceptionHandler);
    if (!s_veh_handle)
    {
      Log_ErrorPrint("Failed to add vectored exception handler");
      return false;
    }
#elif defined(USE_SIGSEGV)
#if 0
    // TODO: Is this needed?
    stack_t signal_stack = {};
    signal_stack.ss_sp = malloc(SIGSTKSZ);
    signal_stack.ss_size = SIGSTKSZ;
    if (sigaltstack(&signal_stack, nullptr))
    {
      Log_ErrorPrintf("signaltstack() failed: %d", errno);
      return false;
    }
#endif

    struct sigaction sa = {};
    sa.sa_sigaction = SIGSEGVHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &s_old_sigsegv_action) < 0)
    {
      Log_ErrorPrintf("sigaction() failed: %d", errno);
      return false;
    }
#else
    return false;
#endif
  }

  return true;
}

bool RemoveHandler(void* owner)
{
  std::lock_guard<std::mutex> guard(m_handler_lock);
  auto it = std::find_if(m_handlers.begin(), m_handlers.end(),
                         [owner](const RegisteredHandler& rh) { return rh.owner == owner; });
  if (it == m_handlers.end())
    return false;

  m_handlers.erase(it);

  if (m_handlers.empty())
  {
#if defined(WIN32)
    RemoveVectoredExceptionHandler(s_veh_handle);
    s_veh_handle = nullptr;
#else
    // restore old signal handler
    if (sigaction(SIGSEGV, &s_old_sigsegv_action, nullptr) < 0)
    {
      Log_ErrorPrintf("sigaction() failed: %d", errno);
      return false;
    }

    s_old_sigsegv_action = {};
#endif
  }

  return true;
}

} // namespace Common::PageFaultHandler
