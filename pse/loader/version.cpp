#include "pse/pse_api.h"

// Keeps pse_core non-empty while the services are being written; also the loader's API version for diagnostics.
extern "C" uint32_t PSE_CALL PSE_LoaderApiVersion(void) { return PSE_API_VERSION; }
