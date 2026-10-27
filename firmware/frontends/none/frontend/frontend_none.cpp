// Frontend for boards without a display (RACEBOX_FRONTEND=none, the default).
#include "bridge_status.h"

namespace bridge {
void frontendStart() {}
void frontendUpdate(const Status&) {}
}  // namespace bridge
