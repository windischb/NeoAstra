#include "native_internal.hpp"

// Backends without a verified filter keep the ABI available, but promise no
// request coverage. The common layer handles an empty filter as a no-op.
uint64_t neo_platform_request_filter_flags(const neoastra_environment_t*) noexcept { return 0; }

neoastra_result_t neo_platform_prepare_request_filter(neoastra_view_t*, const neo_request_filter::filter&,
    neo_request_filter_prepared_callback_t, void*, neoastra_error_t** error) noexcept {
    return neo_fail(error, NEOASTRA_ERROR_NOT_SUPPORTED, "Request filtering is not implemented by this backend");
}
