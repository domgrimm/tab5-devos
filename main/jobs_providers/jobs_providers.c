/* jobs_providers: register the first-slice Jobs action providers. Called from
 * main after devos_actions is ready, before Jobs validates any definition. */
#include "jobs_providers.h"

void jobs_providers_register_all(void)
{
    jobs_system_register();     /* system.log, system.notify */
    jobs_http_register();       /* http.request (existing devos_http worker) */
    jobs_network_register();    /* network.ping (request-specific probe) */
}
