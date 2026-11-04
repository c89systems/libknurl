#ifndef ROTMOS_XML_MAPPING_INTERNAL_H
#define ROTMOS_XML_MAPPING_INTERNAL_H
#include <rotmos/xml.h>
#include "json_internal.h"
rotmos_status rotmos_xml_effective_config(const rotmos_xml_config *,
                                          rotmos_xml_config *, rotmos_error *);
lonejson *rotmos_xml_lonejson_new(const rotmos_xml_config *, rotmos_error *);
#endif
