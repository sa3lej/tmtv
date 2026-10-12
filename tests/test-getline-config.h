/* Force the portable getline into a glibc build without colliding with
 * libc's getdelim declaration. Production fallback source stays unchanged.
 * Use with ac_cv_func_getline=no and CPPFLAGS='-include <this file>'.
 */
#include <stdio.h>
#define getdelim tmtv_test_getdelim
