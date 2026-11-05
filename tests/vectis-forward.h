#ifndef ROTMOS_TEST_VECTIS_FORWARD_H
#define ROTMOS_TEST_VECTIS_FORWARD_H
#include "vectis-route.h"
#include <rotmos/lonejson.h>
#include <rotmos/xml.h>
reference_status reference_json_validate_cstr(const char *, reference_error *);
reference_status
reference_array_each_source(const struct reference_input_source *, const char *,
                            const lonejson_map *, void *, reference_array_item,
                            void *, reference_error *);
struct reference_sink {
    int (*write)(struct reference_sink *, const void *, size_t,
                 struct reference_source_error *);
    void (*close)(struct reference_sink *);
    void *impl;
};
/* Same value layout as vectis_xml_config; codecs remain in Core. */
typedef rotmos_xml_config reference_xml_options;
#define reference_xml_options_init rotmos_xml_config_init
reference_status
reference_xml_parse_source(const struct reference_input_source *,
                           const lonejson_map *, const reference_xml_options *,
                           void *, reference_error *);
reference_status reference_xml_parse(struct reference_source *,
                                     const lonejson_map *,
                                     const reference_xml_options *, void *,
                                     reference_error *);
reference_status reference_xml_write(struct reference_sink *,
                                     const lonejson_map *,
                                     const reference_xml_options *,
                                     const void *, reference_error *);
reference_status reference_xml_bytes(const lonejson_map *,
                                     const reference_xml_options *,
                                     const void *,
                                     struct reference_mutable_bytes *,
                                     reference_error *);
reference_status
reference_array_rewrite_source(const struct reference_input_source *,
                               const char *, struct reference_sink *,
                               const lonejson_array_rewrite_options *,
                               reference_error *);

/* Baseline declaration shapes, including explicit map/storage sizes. These are
 * test-only consumer declarations, never a second shipped server API. */
typedef reference_status (*reference_json_handler)(struct reference_app *,
                                                   struct reference_request *,
                                                   void *, void *, void *,
                                                   reference_error *);
typedef reference_status (*reference_json_typed_handler)(
    struct reference_app *, struct reference_request *, void *,
    struct reference_response *, void *, reference_error *);
typedef reference_json_typed_handler reference_xml_handler;
struct reference_json_config {
    int method;
    unsigned int methods;
    const char *path;
    int path_kind;
    struct reference_body_policy body;
    const lonejson_map *input_map;
    size_t input_size;
    const lonejson_map *output_map;
    size_t output_size;
    reference_json_handler handler;
    void *userdata;
};
struct reference_json_typed_config {
    int method;
    unsigned int methods;
    const char *path;
    int path_kind;
    struct reference_body_policy body;
    const lonejson_map *input_map;
    size_t input_size;
    reference_json_typed_handler handler;
    void *userdata;
};
struct reference_xml_config {
    int method;
    unsigned int methods;
    const char *path;
    int path_kind;
    struct reference_body_policy body;
    size_t buffer_bytes;
    const lonejson_map *input_map;
    size_t input_size;
    rotmos_xml_config config;
    reference_xml_handler handler;
    void *userdata;
};
/* Retain one host association per distinct handler/userdata registration.
 * Optional Core hooks/records/runtime are fixture
 * instrumentation for ownership/order invariants, not Vectis config fields. */
struct reference_typed_route {
    struct reference_app *app;
    reference_json_handler json;
    reference_json_typed_handler typed;
    void *userdata;
    lonejson *runtime;
    rotmos_route_hooks hooks;
    const rotmos_record_provider *records;
};
reference_status reference_json_register(struct reference_typed_route *,
                                         const struct reference_json_config *,
                                         reference_error *);
reference_status
reference_json_typed_register(struct reference_typed_route *,
                              const struct reference_json_typed_config *,
                              reference_error *);
reference_status reference_xml_register(struct reference_typed_route *,
                                        const struct reference_xml_config *,
                                        reference_error *);
reference_status reference_json_reply(struct reference_response *, int,
                                      const lonejson_map *, const void *,
                                      reference_error *);
reference_status reference_json_reply_status(struct reference_response *, int,
                                             reference_error *);
reference_status reference_response_json_generated(struct reference_response *,
                                                   int, const lonejson_map *,
                                                   const void *,
                                                   reference_error *);
#endif
