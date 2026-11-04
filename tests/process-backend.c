#include "process-backend.h"
#include "library.h"

void rotmos_test_child_budget(unsigned long milliseconds)
{
    kore_library_worker_deadline((u_int32_t)milliseconds);
}
