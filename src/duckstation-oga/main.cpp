#include "common/assert.h"
#include "common/log.h"
#include "core/system.h"
#include "oga_host_interface.h"
#include <cstring>

int main(int argc, char* argv[])
{
  Log::SetConsoleOutputParams(true);

  // set log flags
#ifdef _DEBUG
  Log::SetFilterLevel(LOGLEVEL_DEBUG);
#else
  Log::SetFilterLevel(LOGLEVEL_INFO);
#endif

  // Log::SetFilterLevel(LOGLEVEL_DEV);
  // Log::SetFilterLevel(LOGLEVEL_PROFILE);

  // parameters
  std::optional<s32> state_index;
  const char* boot_filename = nullptr;
  for (int i = 1; i < argc; i++)
  {
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

    if (CHECK_ARG_PARAM("-state"))
      state_index = std::atoi(argv[++i]);
    if (CHECK_ARG_PARAM("-resume"))
      state_index = -1;
    else
      boot_filename = argv[i];

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
  }

  // create display and host interface
  std::unique_ptr<OGAHostInterface> host_interface = OGAHostInterface::Create();
  if (!host_interface)
  {
    Panic("Failed to create host interface");
    return -1;
  }

  // boot/load state
  if (boot_filename)
  {
    if (host_interface->BootSystemFromFile(boot_filename) && state_index.has_value())
      host_interface->LoadState(false, state_index.value());
  }
  else if (state_index.has_value())
  {
    host_interface->LoadState(true, state_index.value());
  }

  // run
  host_interface->Run();

  // done
  host_interface.reset();
  return 0;
}
