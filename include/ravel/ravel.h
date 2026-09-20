#ifndef RAVEL_H
#define RAVEL_H

/* C ABI surface — for FFI from languages/engines that can't link C++
 * directly. Mirrors the subset of ravel::Simulation needed to run a
 * simulation and read back a pass/fail + seed. Opaque handle, no exceptions
 * cross this boundary: every function returns a status code instead. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(RAVEL_SHARED) && defined(_WIN32)
#  ifdef RAVEL_BUILD_SHARED
#    define RAVEL_API __declspec(dllexport)
#  else
#    define RAVEL_API __declspec(dllimport)
#  endif
#else
#  define RAVEL_API
#endif

typedef struct ravel_simulation ravel_simulation;  // NOLINT(modernize-use-using): C header

RAVEL_API ravel_simulation* ravel_simulation_create(uint64_t seed);
RAVEL_API void ravel_simulation_destroy(ravel_simulation* sim);
RAVEL_API int ravel_simulation_run(ravel_simulation* sim); /* 1 = ok, 0 = failed */
RAVEL_API const char* ravel_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* RAVEL_H */
