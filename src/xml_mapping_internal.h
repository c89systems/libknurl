#ifndef KNURL_XML_MAPPING_INTERNAL_H
#define KNURL_XML_MAPPING_INTERNAL_H
#include <knurl/xml.h>
#include "json_internal.h"
knurl_status knurl_xml_effective_config(const knurl_xml_config *,
                                        knurl_xml_config *, knurl_error *);
lonejson *knurl_xml_lonejson_new(const knurl_xml_config *, knurl_error *);
#endif
