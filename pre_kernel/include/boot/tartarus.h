#pragma once
#include <tartarus.h>

[[noreturn, gnu::used]] void prekernel_entry_tartarus(tartarus_boot_info_t* boot_info, uint16_t version);
