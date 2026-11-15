#ifndef KNURL_RECEIVERS_INTERNAL_H
#define KNURL_RECEIVERS_INTERNAL_H

struct knurl_server;
void knurl_server_receiver_init(struct knurl_server *receiver);
struct knurl_router;
void knurl_router_receiver_init(struct knurl_router *receiver);
struct knurl_route_group;
void knurl_route_group_receiver_init(struct knurl_route_group *receiver);
struct knurl_route;
void knurl_route_receiver_init(struct knurl_route *receiver);
struct knurl_request;
void knurl_request_receiver_init(struct knurl_request *receiver);
struct knurl_body;
void knurl_body_receiver_init(struct knurl_body *receiver);
struct knurl_channel;
void knurl_channel_receiver_init(struct knurl_channel *receiver);
struct knurl_websocket;
void knurl_websocket_receiver_init(struct knurl_websocket *receiver);
struct knurl_response;
void knurl_response_receiver_init(struct knurl_response *receiver);
struct knurl_reply;
void knurl_reply_receiver_init(struct knurl_reply *receiver);
struct knurl_source;
void knurl_source_receiver_init(struct knurl_source *receiver);
struct knurl_loop;
void knurl_loop_receiver_init(struct knurl_loop *receiver);
struct knurl_watch;
void knurl_watch_receiver_init(struct knurl_watch *receiver);
struct knurl_timer;
void knurl_timer_receiver_init(struct knurl_timer *receiver);
struct knurl_waker;
void knurl_waker_receiver_init(struct knurl_waker *receiver);
struct knurl_asset;
void knurl_asset_receiver_init(struct knurl_asset *receiver);
struct knurl_asset_provider;
void knurl_asset_provider_receiver_init(struct knurl_asset_provider *receiver);
struct knurl_storage_provider;
void knurl_storage_provider_receiver_init(
    struct knurl_storage_provider *receiver);
struct knurl_storage_operation;
void knurl_storage_operation_receiver_init(
    struct knurl_storage_operation *receiver);
struct knurl_storage_resource;
void knurl_storage_resource_receiver_init(
    struct knurl_storage_resource *receiver);
struct knurl_xml_document;
void knurl_xml_document_receiver_init(struct knurl_xml_document *receiver);
struct knurl_xml_node;
void knurl_xml_node_receiver_init(struct knurl_xml_node *receiver);
struct knurl_xml_sax;
void knurl_xml_sax_receiver_init(struct knurl_xml_sax *receiver);
struct knurl_xml_generator;
void knurl_xml_generator_receiver_init(struct knurl_xml_generator *receiver);

#endif
