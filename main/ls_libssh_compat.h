/* Forced into every libssh translation unit (see the LS_SSH block in the
   top-level CMakeLists.txt).

   libssh's port/idf_compat.h defines NI_NUMERICHOST as lwIP's AI_NUMERICHOST,
   but the getnameinfo() libssh ends up calling is sock_utils', which checks
   its flags against the NI_* values in its own netdb_macros.h and returns
   EAI_BADFLAGS for anything else. Include the port header first (it is
   pragma-once, so later includes are no-ops) and put sock_utils' values back. */
#pragma once
#include "idf_compat.h"
#undef NI_NUMERICHOST
#undef NI_NUMERICSERV
#include "lwip/netdb.h"
#include "netdb_macros.h"
