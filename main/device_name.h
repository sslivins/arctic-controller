#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool device_name_validate_and_normalize(const char* input,
                                        char* output,
                                        size_t output_len,
                                        char* error,
                                        size_t error_len);

#ifdef __cplusplus
}
#endif
