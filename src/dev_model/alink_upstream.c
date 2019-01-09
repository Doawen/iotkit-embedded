/*
 * Copyright (C) 2015-2018 Alibaba Group Holding Limited
 */

#include "iotx_alink_internal.h"
#include "alink_wrapper.h"


#define ALINK_REQ_LIST_NUM_MAX      50


#if (CONFIG_SDK_THREAD_COST != 0)
alink_upstream_req_ctx_t alink_upstream_req_ctx = { 0 };

static void _alink_upstream_req_list_lock(void)
{
    if (alink_upstream_req_ctx.mutex) {
        HAL_MutexLock(alink_upstream_req_ctx.mutex);
    }
}
static void _alink_upstream_req_list_unlock(void)
{
    if (alink_upstream_req_ctx.mutex) {
        HAL_MutexUnlock(alink_upstream_req_ctx.mutex);
    }
}

int alink_upstream_req_ctx_init(void)
{
    alink_upstream_req_ctx.mutex = HAL_MutexCreate();
    if (alink_upstream_req_ctx.mutex == NULL) {
        return FAIL_RETURN;
    }

    alink_upstream_req_ctx.list_num = 0;
    INIT_LIST_HEAD(&alink_upstream_req_ctx.req_list);

    return SUCCESS_RETURN;
}

int alink_upstream_req_ctx_deinit(void)
{
    alink_upstream_req_node_t *search_node, *next;

    _alink_upstream_req_list_lock();
    list_for_each_entry_safe(search_node, next, &alink_upstream_req_ctx.req_list, list, alink_upstream_req_node_t) {
        if (search_node->data) {
            alink_free(search_node->data);
        }
        list_del(&search_node->list);
        alink_free(search_node);
    }
    alink_upstream_req_ctx.list_num = 0;
    _alink_upstream_req_list_unlock();

    if (alink_upstream_req_ctx.mutex) {
        HAL_MutexDestroy(alink_upstream_req_ctx.mutex);
    }

    return SUCCESS_RETURN;
}

int alink_upstream_req_list_insert(uint32_t msgid, uint32_t devid, alink_msg_uri_index_t msg_uri, void *data)
{
    alink_upstream_req_node_t *node;

    alink_info("upstream req list num = %d", alink_upstream_req_ctx.list_num);
    if (alink_upstream_req_ctx.list_num >= ALINK_REQ_LIST_NUM_MAX) {
        node = list_entry(alink_upstream_req_ctx.req_list.next, alink_upstream_req_node_t, list);
        list_del(&node->list);
        alink_free(node);
        alink_warning("response packet will be abandoned");
        alink_upstream_req_ctx.list_num--;
        /*return FAIL_RETURN; TODO!*/
    }

    node = alink_malloc(sizeof(alink_upstream_req_node_t));
    if (node == NULL) {
        return IOTX_CODE_MEMORY_NOT_ENOUGH;
    }

    node->msgid = msgid;
    node->devid = devid;
    node->msg_uri = msg_uri;
    node->data = data;
    INIT_LIST_HEAD(&node->list);

    _alink_upstream_req_list_lock();

    list_add_tail(&node->list, &alink_upstream_req_ctx.req_list);
    alink_upstream_req_ctx.list_num++;

    _alink_upstream_req_list_unlock();

    alink_info("upstream req add list succeed");

    return SUCCESS_RETURN;
}

int alink_upstream_req_list_search(uint32_t msgid, alink_upstream_req_node_t **node)
{
    alink_upstream_req_node_t *search_node, *next;
    uint8_t idx = 0;

    ALINK_ASSERT_DEBUG(node != NULL);

    _alink_upstream_req_list_lock();

    list_for_each_entry_safe(search_node, next, &alink_upstream_req_ctx.req_list, list, alink_upstream_req_node_t) {
        alink_info("req_list idx %d, msgid = %d", idx, search_node->msgid);
        if (search_node->msgid == msgid) {
            *node = search_node;
            _alink_upstream_req_list_unlock();
            alink_info("corresponding req exist");
            return SUCCESS_RETURN;
        }

        if (msgid > search_node->msgid && (msgid - search_node->msgid) > 25) {  /* TODO */
            list_del(&search_node->list);
            alink_free(search_node);
            alink_upstream_req_ctx.list_num--;
        }
    }

    _alink_upstream_req_list_unlock();
    alink_info("corresponding req no exist");
    return FAIL_RETURN;
}

int alink_upstream_req_list_delete_by_msgid(int msgid)
{
    alink_upstream_req_node_t *search_node, *next;

    _alink_upstream_req_list_lock();
    list_for_each_entry_safe(search_node, next, &alink_upstream_req_ctx.req_list, list, alink_upstream_req_node_t) {
        if (search_node->msgid == msgid) {
            list_del(&search_node->list);
            alink_free(search_node);
            alink_upstream_req_ctx.list_num--;
            _alink_upstream_req_list_unlock();
            return SUCCESS_RETURN;
        }
    }

    _alink_upstream_req_list_unlock();
    return FAIL_RETURN;
}
 
int alink_upstream_req_list_delete_by_node(alink_upstream_req_node_t *node)
{
    if (node == NULL) {
        return IOTX_CODE_PARAMS_INVALID;
    }

    _alink_upstream_req_list_lock();

    list_del(&node->list);
    alink_free(node);
    alink_upstream_req_ctx.list_num--;

    _alink_upstream_req_list_unlock();
    return SUCCESS_RETURN;
}
#endif


static int _alink_upstream_send_request_msg(alink_msg_uri_index_t idx, uint32_t devid, 
                                            const uint8_t *payload, uint32_t len, alink_uri_query_t *query)
{
    int res = FAIL_RETURN;
    char *uri;
    uint32_t msgid = 0;
    char uri_query[45] = {0};

    /* msgid is generated internally */
    msgid = alink_core_allocate_msgid();

    if (query == NULL) {
        HAL_Snprintf(uri_query, sizeof(uri_query), "/?i=%d", msgid);
    }
    else {
        query->id = msgid;
        alink_format_assemble_query(query, uri_query, sizeof(uri_query));
    }

    /* get specific uri of idx */
    if (devid == 0) {
        res = alink_format_get_upstream_complete_uri(idx, uri_query, &uri);
    }
    else {
#ifdef DEVICE_MODEL_GATEWAY
        char product_key[IOTX_PRODUCT_KEY_LEN] = { 0 };
        char device_name[IOTX_DEVICE_NAME_LEN] = { 0 };

        /* get subdev status first, TODO */
        res = alink_subdev_get_pkdn_by_devid(devid, product_key, device_name);
        if (res < SUCCESS_RETURN) {
            return IOTX_CODE_SUBDEV_NOT_EXIST;
        }

        res = alink_format_get_upstream_subdev_complete_url(idx, product_key, device_name, uri_query, &uri);
#else
        return IOTX_CODE_GATEWAY_UNSUPPORTED;
#endif        
    }

    if (res < 0) {
        return res;
    }    

    /* parse the payload if it's json format */
    if (query == NULL || query->format != 'b') {
        lite_cjson_t lite;
        memset(&lite, 0, sizeof(lite_cjson_t));
        res = lite_cjson_parse((char *)payload, len, &lite);
        if (res < SUCCESS_RETURN) {
            alink_err("wrong json format: %.*s", len, payload);
            alink_free(uri);
            return FAIL_RETURN;
        }
    }

    res = alink_core_send_req_msg(uri, payload, len);
    alink_free(uri);

    alink_debug("core send req return = %d", res);

    if (res == SUCCESS_RETURN) {
        res = msgid;
    }

    return res;
}

static int _alink_upstream_send_response_msg(alink_msg_uri_index_t idx, const char *pk, const char *dn, 
                                             const uint8_t *payload, uint32_t len, alink_uri_query_t *query)
{
    int res = FAIL_RETURN;
    char *uri;
    char uri_query[45] = {0};

    ALINK_ASSERT_DEBUG(query);

    /* response msg doesn't need ack */
    query->ack = '\0';
    alink_format_assemble_query(query, uri_query, sizeof(uri_query));

    /* get specific uri of idx */
    if (*pk == '\0' && *dn == '\0') {
        res = alink_format_get_upstream_complete_uri(idx, uri_query, &uri);
    }
    else {
#ifdef DEVICE_MODEL_GATEWAY
        res = alink_format_get_upstream_subdev_complete_url(idx, pk, dn, uri_query, &uri);
#else
        return IOTX_CODE_GATEWAY_UNSUPPORTED;
#endif        
    }

    if (res < 0) {
        return res;
    }    

    /* parse the payload if it's json format */
    if (query->format != 'b') {
        lite_cjson_t lite;
        memset(&lite, 0, sizeof(lite_cjson_t));
        res = lite_cjson_parse((char *)payload, len, &lite);
        if (res < SUCCESS_RETURN) {
            alink_err("wrong json format: %.*s", len, payload);
            alink_free(uri);
            return FAIL_RETURN;
        }
    }

    res = alink_core_send_req_msg(uri, payload, len);
    alink_free(uri);

    alink_debug("core send rsp return = %d", res);
    return res;
}

/***************************************************************
 * device model management upstream message
 ***************************************************************/
const char *c_property_post_fmt = "{\"p\":%.*s}";
int alink_upstream_thing_property_post_req(uint32_t devid, const char *user_data, uint32_t data_len)
{
    int res = FAIL_RETURN;

    uint32_t len = strlen(c_property_post_fmt) + data_len;
    char *payload = alink_malloc(len);
    if (payload == NULL) {
        return IOTX_CODE_MEMORY_NOT_ENOUGH;
    }
    
    memset(payload, 0, len);
    HAL_Snprintf(payload, len, c_property_post_fmt, data_len, user_data);
    res = _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_PROPERTY_POST, devid, (uint8_t *)payload, strlen(payload), NULL);
    alink_free(payload);

    return res;
}

const char *c_event_post_fmt = "{\"id\":\"%.*s\",\"params\":%.*s}";
/**
 * thing model post event to cloud 
 */
int alink_upstream_thing_event_post_req(uint32_t devid, const char *event_id, uint8_t id_len, const char *user_data, uint32_t data_len)
{
    int res = FAIL_RETURN;

    /* alink payload format */
    uint32_t len = strlen(c_event_post_fmt) + id_len + data_len;

    char *payload = alink_malloc(len);
    if (payload == NULL) {
        return IOTX_CODE_MEMORY_NOT_ENOUGH;
    }

    memset(payload, 0, len);
    HAL_Snprintf(payload, len, c_event_post_fmt, id_len, event_id, data_len, user_data);
    res = _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_EVENT_POST, devid, (uint8_t *)payload, strlen(payload), NULL);
    alink_free(payload);

    return res;
}

static int _alink_upstream_empty_rsp(alink_msg_uri_index_t idx, const char *pk, const char *dn, alink_uri_query_t *query)
{
    char payload[3] = "{}";
    
    return _alink_upstream_send_response_msg(idx, pk, dn, (uint8_t *)payload, strlen(payload), query);
}

int alink_upstream_thing_property_set_rsp(const char *pk, const char *dn, alink_uri_query_t *query)      /* TODO */
{
    return _alink_upstream_empty_rsp(ALINK_URI_UP_RSP_PROPERTY_PUT, pk, dn, query);
}

const char *c_property_get_rsp_fmt = "{\"code\":%d,\"params\":%.*s}";
int alink_upstream_thing_property_get_rsp(const char *pk, const char *dn, uint32_t code, const char *user_data, uint32_t data_len, alink_uri_query_t *query)
{
    int res = FAIL_RETURN;

    /* alink payload format */
    uint32_t len = strlen(c_property_get_rsp_fmt) + 10 + data_len;

    char *payload = alink_malloc(len);
    if (payload == NULL) {
        return IOTX_CODE_MEMORY_NOT_ENOUGH;
    }

    memset(payload, 0, len);
    HAL_Snprintf(payload, len, c_property_get_rsp_fmt, code, data_len, user_data);
    res = _alink_upstream_send_response_msg(ALINK_URI_UP_RSP_PROPERTY_GET, pk, dn, (uint8_t *)payload, strlen(payload), query);
    alink_free(payload);

    return res;
}

/** TODO: same with property get rsp */
const char *c_service_invoke_rsp_fmt = "{\"id\":\"%s\",\"params\":%.*s}";
int alink_upstream_thing_service_invoke_rsp(const char  *pk, const char *dn, const char *service_id, const char *user_data, uint32_t data_len, alink_uri_query_t *query)
{
    int res = FAIL_RETURN;

    /* alink payload format */
    uint32_t len = strlen(c_service_invoke_rsp_fmt) + strlen(service_id) + data_len;

    char *payload = alink_malloc(len);
    if (payload == NULL) {
        return IOTX_CODE_MEMORY_NOT_ENOUGH;
    }

    memset(payload, 0, len);
    HAL_Snprintf(payload, len, c_service_invoke_rsp_fmt, service_id, data_len, user_data);
    res = _alink_upstream_send_response_msg(ALINK_URI_UP_RSP_SERVICE_PUT, pk, dn, (uint8_t *)payload, strlen(payload), query);
    alink_free(payload);

    return res;
}

/***************************************************************
 * device model management raw data mode upstream message
 ***************************************************************/
int alink_upstream_thing_raw_post_req(uint32_t devid, const uint8_t *user_data, uint32_t data_len)
{
    alink_uri_query_t query;
    query.format = 'b';         /* setup to binary format */

    return _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_RAW_POST, devid, user_data, data_len, &query);
}

#if 0 /** this function is useless, as the script doesn't care about which topic the raw data come from **/
int alink_upstream_thing_raw_post_rsp(const char *pk, const char *dn, const uint8_t *user_data, uint32_t data_len)
{
    return 0;
}
#endif

#ifdef DEVICE_MODEL_GATEWAY
/***************************************************************
 * subdevice management upstream message
 ***************************************************************/
char *_alink_upstream_assamble_pkdn_pair_payload(alink_subdev_id_list_t *subdev_list)
{
    uint8_t idx = 0;
    uint8_t sudev_num = subdev_list->subdev_num;
    uint32_t *subdev_id_array = subdev_list->subdev_array;
    lite_cjson_item_t *lite_root = NULL, *lite_array = NULL, *lite_array_item = NULL;
    char *payload = NULL;

    lite_root = lite_cjson_create_object();
    if (lite_root == NULL) {
        return NULL;
    }
    
    lite_array = lite_cjson_create_array();
    if (lite_array == NULL) {
        lite_cjson_delete(lite_root);
        return NULL;
    }

    lite_cjson_add_item_to_object(lite_root, "subList", lite_array);

    for (idx = 0; idx < sudev_num; idx++) {
        char pk[IOTX_PRODUCT_KEY_LEN] = {0};
        char dn[IOTX_DEVICE_NAME_LEN] = {0};

        alink_subdev_get_pkdn_by_devid(subdev_id_array[idx], pk, dn);

        /* check parameters */
        if (*pk == '\0' || *dn == '\0') {
            lite_cjson_delete(lite_root);
            return NULL;
        }

        lite_array_item = lite_cjson_create_object();
        if (lite_array_item == NULL) {
            lite_cjson_delete(lite_root);
            return NULL;
        }

        lite_cjson_add_string_to_object(lite_array_item, "productKey", pk);
        lite_cjson_add_string_to_object(lite_array_item, "deviceName", dn);
        lite_cjson_add_item_to_array(lite_array, lite_array_item);
    }

    payload = lite_cjson_print_unformatted(lite_root);
    lite_cjson_delete(lite_root);

    return payload;
}

const char *c_login_sign_source_fmt = "clientId%sdeviceName%sproductKey%stimestamp%s";   /* clientId is pk.dn */
char *_alink_upstream_assamble_auth_list_payload(alink_subdev_id_list_t *subdev_list)
{
    uint8_t idx = 0;
    uint8_t subdev_num = subdev_list->subdev_num;
    uint32_t *subdev_id_array = subdev_list->subdev_array;
    lite_cjson_item_t *lite_root = NULL, *lite_array = NULL, *lite_array_item = NULL;
    char *payload = NULL;

    lite_root = lite_cjson_create_object();
    if (lite_root == NULL) {
        return NULL;
    }
    
    lite_array = lite_cjson_create_array();
    if (lite_array == NULL) {
        lite_cjson_delete(lite_root);
        return NULL;
    }

    lite_cjson_add_item_to_object(lite_root, "subList", lite_array);

    for (idx = 0; idx < subdev_num; idx++) {
        char timestamp[20] = {0};
        char clientid[IOTX_PRODUCT_KEY_LEN + IOTX_DEVICE_NAME_LEN + 2];
        char sign[32] = {0};
        char sign_string[65] = {0};
        char *sign_source;
        uint16_t sign_source_len;
        char pk[IOTX_PRODUCT_KEY_LEN] = {0};
        char dn[IOTX_DEVICE_NAME_LEN] = {0};
        char ds[IOTX_DEVICE_SECRET_LEN] = {0};

        alink_subdev_get_triple_by_devid(subdev_id_array[idx], pk, dn, ds);

        /* check parameters */
        if (*ds == '\0') {
            lite_cjson_delete(lite_root);
            return NULL;
        }

        lite_array_item = lite_cjson_create_object();
        if (lite_array_item == NULL) {
            lite_cjson_delete(lite_root);
            return NULL;
        }

        HAL_Snprintf(clientid, sizeof(clientid), "%s.%s", pk, dn);
        HAL_Snprintf(timestamp, sizeof(timestamp), "%llu", HAL_UptimeMs());
        sign_source_len = strlen(c_login_sign_source_fmt) + strlen(clientid) + strlen(pk) + strlen(dn) + strlen(timestamp);
        sign_source = alink_malloc(sign_source_len);
        HAL_Snprintf(sign_source, sign_source_len, c_login_sign_source_fmt, clientid, pk, dn, timestamp);
        utils_hmac_sha256((const uint8_t *)sign_source, strlen(sign_source), (const uint8_t *)ds, strlen(ds), (uint8_t *)sign);
        infra_hex2str((uint8_t *)sign, 32, sign_string);
        alink_info("%s", sign_source);
        alink_free(sign_source);

        lite_cjson_add_string_to_object(lite_array_item, "ci", clientid);
        lite_cjson_add_string_to_object(lite_array_item, "dn", dn);
        lite_cjson_add_string_to_object(lite_array_item, "pk", pk);
        lite_cjson_add_string_to_object(lite_array_item, "ts", timestamp);
        lite_cjson_add_string_to_object(lite_array_item, "sm", "hmacSha256");
        lite_cjson_add_string_to_object(lite_array_item, "sn", sign_string);

        lite_cjson_add_item_to_array(lite_array, lite_array_item);
    }

    payload = lite_cjson_print_unformatted(lite_root);
    lite_cjson_delete(lite_root);

    return payload;
}

int alink_upstream_subdev_register_post_req(alink_subdev_id_list_t *subdev_list)
{
    int res = FAIL_RETURN;
    char *payload = NULL;

    payload = _alink_upstream_assamble_pkdn_pair_payload(subdev_list);
    if (payload == NULL) {
        return res;
    }

    res = _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_SUB_REGISTER_POST, 0, (uint8_t *)payload, strlen(payload), NULL);
    alink_free(payload);

    alink_info("subdev reg_post msgid = %d", res);

#if (CONFIG_SDK_THREAD_COST != 0)
    /* TODO: subdev num should < 10 */
    {
        upstream_req_data_subdv_list_t *req_data = alink_malloc(sizeof(upstream_req_data_subdv_list_t));
        if (req_data == NULL) {
            return IOTX_CODE_MEMORY_NOT_ENOUGH;
        }
        memcpy(req_data->subdev_id, subdev_list->subdev_array, sizeof(uint32_t ) * (subdev_list->subdev_num));

        /* todo */
        alink_upstream_req_list_insert(res, 0, ALINK_URI_UP_REQ_SUB_REGISTER_POST, req_data);   /* res is msgid if post succeed */
    }
#endif

    return res;
}

int alink_upstream_subdev_register_delete_req(alink_subdev_id_list_t *subdev_list)
{
    int res = FAIL_RETURN;
    char *payload = NULL;

    payload = _alink_upstream_assamble_pkdn_pair_payload(subdev_list);
    if (payload == NULL) {
        return res;
    }

    res = _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_SUB_REGISTER_DELETE, 0, (uint8_t *)payload, strlen(payload), NULL);
    alink_free(payload);
    alink_info("subdev reg_delete msgid = %d", res);

    return res;
}

int alink_upstream_subdev_login_post_req(alink_subdev_id_list_t *subdev_list)
{
    int res = FAIL_RETURN;
    char *payload = NULL;

    payload = _alink_upstream_assamble_auth_list_payload(subdev_list);
    if (payload == NULL) {
        alink_info("subdev login_post assamble payload failed");
        return res;
    }

    res = _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_SUB_LOGIN_POST, 0, (uint8_t *)payload, strlen(payload), NULL);
    alink_free(payload);
    alink_info("subdev login_post msgid = %d", res);

    return res;
}

int alink_upstream_subdev_login_delete_req(alink_subdev_id_list_t *subdev_list)
{
    int res = FAIL_RETURN;
    char *payload = NULL;

    payload = _alink_upstream_assamble_pkdn_pair_payload(subdev_list);
    if (payload == NULL) {
        return res;
    }

    res = _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_SUB_LOGIN_DELETE, 0, (uint8_t *)payload, strlen(payload), NULL);
    alink_free(payload);
    alink_info("subdev login_delete msgid = %d", res);

    return res;
}

#if 0 /** all topo relatived funcitons are not implement **/
int alink_upstream_thing_topo_post(void)
{

}

int alink_upstream_thing_topo_delete(void)
{

}

int alink_upstream_thing_topo_get(void)
{

}

int alink_upstream_subdev_list_post_req(void)
{

}

int alink_upstream_subdev_list_put_rsp(void)
{

}
#endif

int alink_upstream_gw_permit_put_rsp(const char *pk, const char *dn, uint32_t code, alink_uri_query_t *query)
{
    return _alink_upstream_empty_rsp(ALINK_URI_UP_RSP_GW_PERMIT_PUT, pk, dn, query);
}

int alink_upstream_gw_config_put_rsp(const char *pk, const char *dn, uint32_t code, alink_uri_query_t *query)
{
    return _alink_upstream_empty_rsp(ALINK_URI_UP_RSP_GW_CONIFG_PUT, pk, dn, query);
}
#endif

/***************************************************************
 * thing device information management upstream message
 ***************************************************************/
const char *c_deviceinfo_alink_fmt = "{\"tagList\":%.*s}";
int alink_upstream_thing_deviceinfo_post_req(uint32_t devid, const char *user_data, uint32_t data_len)
{
    int res = FAIL_RETURN;

    uint32_t len = strlen(c_deviceinfo_alink_fmt) + data_len;
    char *payload = alink_malloc(len);
    if (payload == NULL) {
        return IOTX_CODE_MEMORY_NOT_ENOUGH;
    }

    memset(payload, 0, len);
    HAL_Snprintf(payload, len, c_deviceinfo_alink_fmt, data_len, user_data);
    res = _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_DEVINFO_POST, devid, (uint8_t *)payload, strlen(payload), NULL);
    alink_free(payload);

    return res;
}

int alink_upstream_thing_deviceinfo_get_req(uint32_t devid)
{
    char payload[] = "{}";
    return _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_DEVINFO_POST, devid, (uint8_t *)payload, strlen(payload), NULL);
}

int alink_upstream_thing_deviceinfo_delete_req(uint32_t devid, const char *user_data, uint32_t data_len)
{
    int res = FAIL_RETURN;

    uint32_t len = strlen(c_deviceinfo_alink_fmt) + data_len;
    char *payload = alink_malloc(len);
    if (payload == NULL) {
        return IOTX_CODE_MEMORY_NOT_ENOUGH;
    }

    memset(payload, 0, len);
    HAL_Snprintf(payload, len, c_deviceinfo_alink_fmt, data_len, user_data);
    res = _alink_upstream_send_request_msg(ALINK_URI_UP_REQ_DEVINFO_DELETE, devid, (uint8_t *)payload, strlen(payload), NULL);
    alink_free(payload);

    return res;
}


