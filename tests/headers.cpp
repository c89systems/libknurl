#include <knurl/knurl.h>
#include <knurl/lonejson.h>
#include <knurl/pslog.h>

int main()
{
    knurl_error error;
    knurl_error_clear(&error);
    return error.status == KNURL_OK ? 0 : 1;
}
