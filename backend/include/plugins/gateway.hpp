#pragma once

#include "proxy/gateway.hpp"

// RevlmPluginSDK v1 currently exposes the gateway and wire model types through
// the same versioned core library as plugins/sdk.hpp. This facade is the public
// include used by official plugins and keeps the include contract explicit.
