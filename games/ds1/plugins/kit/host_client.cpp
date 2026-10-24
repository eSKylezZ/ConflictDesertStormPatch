// The dsfix kit's host client: the features::X / net::X / padio::X functions a moved feature calls in dsfix.dll,
// generated from src/core/dsfix_host.spec (scripts/gen_host_calls.py), each forwarding to dsfix.dll's interface
// dsfix.host. Compiled into every kit plugin (dsfix_add_plugin) rather than the static kit library: a plugin that
// implements some of these itself (its <group>.spec) has them listed in its generated own_functions.h, which leaves
// them out here, so its own definitions don't clash.
#include <windows.h>

#include <string>
#include <vector>

#include "core/net.h"
#include "features/features.h"
#include "features/padio.h"
#include "generated/dsfix_host.h"
#include "kit/kit.h"

#if __has_include("own_functions.h")
#include "own_functions.h"
#endif

#include "generated/dsfix_host_client.inc"
