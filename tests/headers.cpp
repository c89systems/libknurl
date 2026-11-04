#include <rotmos/rotmos.h>
#include <rotmos/lonejson.h>
#include <rotmos/pslog.h>

int main()
{
    rotmos_error error;
    rotmos_error_clear(&error);
    return error.status == ROTMOS_OK ? 0 : 1;
}
