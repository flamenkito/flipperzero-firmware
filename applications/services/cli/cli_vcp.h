/**
 * @file cli_vcp.h
 * VCP HAL API
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_CLI_VCP "cli_vcp"

typedef struct CliVcp CliVcp;

typedef struct {
    bool (*set_override)(CliVcp* cli_vcp, bool enabled);
    bool (*get_override)(CliVcp* cli_vcp);
    bool (*is_enabled)(CliVcp* cli_vcp);
    void (*disable_for_lock)(CliVcp* cli_vcp);
    void (*retry_lock_convergence)(CliVcp* cli_vcp);
    bool (*try_install_boot_identity)(CliVcp* cli_vcp, bool only_if_currently_null);
} CliVcpInternalApi;

static inline const CliVcpInternalApi* cli_vcp_get_internal_api(CliVcp* cli_vcp) {
    return *(const CliVcpInternalApi* const*)cli_vcp;
}

void cli_vcp_enable(CliVcp* cli_vcp);
void cli_vcp_disable(CliVcp* cli_vcp);

/** Set volatile user intent for CDC; actual state and the observed HAL interface remain separate. */
static inline bool cli_vcp_set_override(CliVcp* cli_vcp, bool enabled) {
    return cli_vcp_get_internal_api(cli_vcp)->set_override(cli_vcp, enabled);
}

/** Get volatile CDC user intent, not actual state or the observed HAL interface. */
static inline bool cli_vcp_get_override(CliVcp* cli_vcp) {
    return cli_vcp_get_internal_api(cli_vcp)->get_override(cli_vcp);
}

/** Get actual CLI CDC state, not user intent or the observed HAL interface. */
static inline bool cli_vcp_is_enabled(CliVcp* cli_vcp) {
    return cli_vcp_get_internal_api(cli_vcp)->is_enabled(cli_vcp);
}

/** Fence CLI I/O and converge a PIN lock to the active boot identity. */
static inline void cli_vcp_disable_for_lock(CliVcp* cli_vcp) {
    cli_vcp_get_internal_api(cli_vcp)->disable_for_lock(cli_vcp);
}

/** Retry a deferred PIN-lock convergence after an advisory-lock race or HAL failure. */
static inline void cli_vcp_retry_lock_convergence(CliVcp* cli_vcp) {
    cli_vcp_get_internal_api(cli_vcp)->retry_lock_convergence(cli_vcp);
}

/** Fence CLI CDC I/O before an external owner changes the observed HAL interface. */
bool cli_vcp_usb_takeover_begin(CliVcp* cli_vcp);

/** End external USB ownership and resume actual CLI state only on an observed CDC interface. */
void cli_vcp_usb_takeover_end(CliVcp* cli_vcp, bool resume);

/** Serialize installation of the latched boot identity with USB takeover requests. */
static inline bool
    cli_vcp_try_install_boot_identity(CliVcp* cli_vcp, bool only_if_currently_null) {
    return cli_vcp_get_internal_api(cli_vcp)
        ->try_install_boot_identity(cli_vcp, only_if_currently_null);
}
#ifdef __cplusplus
}
#endif
