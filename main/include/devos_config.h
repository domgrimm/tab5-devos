#pragma once
/* Single source of truth lives in the devos_config component; this forwarder
 * keeps "devos_config.h" resolving to it from main/ and the simulator build
 * (two independent copies had already drifted apart). */
#include "../../components/devos_config/include/devos_config.h"
