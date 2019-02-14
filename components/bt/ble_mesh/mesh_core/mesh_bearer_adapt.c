/*
 * Copyright (c) 2017 Nordic Semiconductor ASA
 * Copyright (c) 2015-2016 Intel Corporation
 * Additional Copyright (c) 2018 Espressif Systems (Shanghai) PTE LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include "mesh_bearer_adapt.h"
#include "mesh_trace.h"

#include "bta/bta_api.h"
#include "bta/bta_gatt_api.h"
#include "bta/bta_gatt_common.h"
#include "stack/btm_ble_api.h"
#include "p_256_ecc_pp.h"

#include "stack/hcimsgs.h"
#include "osi/future.h"
#include "include/mesh_buf.h"
#include "osi/allocator.h"
#include "include/mesh_atomic.h"
#include "include/mesh_hci.h"

#include "mbedtls/aes.h"
#include "mesh_aes_encrypt.h"
#include "sdkconfig.h"
#if CONFIG_BT_MESH

#include "bta/bta_api.h"
#include <errno.h>
#include "include/mesh_buf.h"
#include "osi/allocator.h"
#include "include/mesh_atomic.h"
#include "bta_gattc_int.h"
#include "provisioner_prov.h"
#include "common.h"

#define BLE_MESH_BTM_CHECK_STATUS(func) do {                                                     \
        tBTM_STATUS __status = (func);                                                           \
        if ((__status != BTM_SUCCESS) && (__status != BTM_CMD_STARTED)) {                        \
            BT_ERR("%s, invalid status %d", __func__, __status);                                 \
            return -1;                                                                           \
        }                                                                                        \
    } while(0);

#define BT_MESH_GATT_GET_CONN_ID(conn_id)               (((u16_t)(conn_id)) >> 8)
#define BT_MESH_GATT_CREATE_CONN_ID(gatt_if, conn_id)   ((u16_t)((((u8_t)(conn_id)) << 8) | ((u8_t)(gatt_if))))
#define CONFIG_BT_MAX_CONN                              CONFIG_BT_ACL_CONNECTIONS

/* We don't need to manage the BT_DEV_ADVERTISING flags in the version of bluedriod, 
 * it will manage it in the BTM layer.
 */
#define BT_DEV  0

/* Global Variables */
extern struct bt_dev bt_dev;

/* P-256 Variables */
static u8_t bt_mesh_public_key[64];
static BT_OCTET32 bt_mesh_private_key = {
    0x3f, 0x49, 0xf6, 0xd4, 0xa3, 0xc5, 0x5f, 0x38,
    0x74, 0xc9, 0xb3, 0xe3, 0xd2, 0x10, 0x3f, 0x50,
    0x4a, 0xff, 0x60, 0x7b, 0xeb, 0x40, 0xb7, 0x99,
    0x58, 0x99, 0xb8, 0xa6, 0xcd, 0x3c, 0x1a, 0xbd
};

/* Scan related functions */
static bt_le_scan_cb_t *bt_mesh_scan_dev_found_cb;
static void bt_mesh_scan_result_callback(tBTA_DM_SEARCH_EVT event, tBTA_DM_SEARCH *p_data);

#if defined(CONFIG_BT_MESH_NODE) && CONFIG_BT_MESH_NODE
/* the gatt database list to save the attribute table */
static sys_slist_t bt_mesh_gatts_db;

/* Static Variables */
static struct bt_conn bt_mesh_gatts_conn[CONFIG_BT_MAX_CONN];
static struct bt_conn_cb *bt_mesh_gatts_conn_cb;
static tBTA_GATTS_IF bt_mesh_gatts_if;
static u16_t svc_handle, char_handle;
static future_t *future_mesh;

/* Static Functions */
static struct bt_gatt_attr *bt_mesh_gatts_find_attr_by_handle(u16_t handle);
#endif /* defined(CONFIG_BT_MESH_NODE) && CONFIG_BT_MESH_NODE */

#if defined(CONFIG_BT_MESH_PROVISIONER) && CONFIG_BT_MESH_PROVISIONER
#define BT_MESH_GATTC_APP_UUID_BYTE     0x97
static struct gattc_prov_info {
    /* Service to be found depends on the type of adv pkt received */
    struct bt_conn conn;
    BD_ADDR addr;
    u8_t  addr_type;
    u16_t service_uuid;
    u16_t mtu;
    bool  wr_desc_done;    /* Indicate if write char descriptor event is received */
    u16_t start_handle;    /* Service attribute start handle */
    u16_t end_handle;      /* Service attribute end handle */
    u16_t data_in_handle;  /* Data In Characteristic attribute handle */
    u16_t data_out_handle; /* Data Out Characteristic attribute handle */
    u16_t ccc_handle;      /* Data Out Characteristic CCC attribute handle */
} bt_mesh_gattc_info[CONFIG_BT_MAX_CONN];
static struct bt_prov_conn_cb *bt_mesh_gattc_conn_cb;
static tBTA_GATTC_IF bt_mesh_gattc_if;
#endif /* defined(CONFIG_BT_MESH_PROVISIONER) && CONFIG_BT_MESH_PROVISIONER */

static void ble_mesh_scan_results_change_2_bta(tBTM_INQ_RESULTS *p_inq, u8_t *p_eir, tBTA_DM_SEARCH_CBACK *p_scan_cback)
{
    tBTA_DM_SEARCH result;
    tBTM_INQ_INFO *p_inq_info;

    bdcpy(result.inq_res.bd_addr, p_inq->remote_bd_addr);
    result.inq_res.rssi = p_inq->rssi;
    result.inq_res.ble_addr_type    = p_inq->ble_addr_type;
    result.inq_res.inq_result_type  = p_inq->inq_result_type;
    result.inq_res.device_type      = p_inq->device_type;
    result.inq_res.flag             = p_inq->flag;
    result.inq_res.adv_data_len     = p_inq->adv_data_len;
    result.inq_res.scan_rsp_len     = p_inq->scan_rsp_len;
    memcpy(result.inq_res.dev_class, p_inq->dev_class, sizeof(DEV_CLASS));
    result.inq_res.ble_evt_type     = p_inq->ble_evt_type;

    /* application will parse EIR to find out remote device name */
    result.inq_res.p_eir = p_eir;

    if ((p_inq_info = BTM_InqDbRead(p_inq->remote_bd_addr)) != NULL) {
        /* initialize remt_name_not_required to FALSE so that we get the name by default */
        result.inq_res.remt_name_not_required = FALSE;
    }

    if (p_scan_cback) {
        p_scan_cback(BTA_DM_INQ_RES_EVT, &result);
    }

    if (p_inq_info) {
        /* application indicates if it knows the remote name, inside the callback
         copy that to the inquiry data base*/
        if (result.inq_res.remt_name_not_required) {
            p_inq_info->appl_knows_rem_name = TRUE;
        }
    }
}

static void ble_mesh_scan_results_cb(tBTM_INQ_RESULTS *p_inq, u8_t *p_eir)
{
    ble_mesh_scan_results_change_2_bta(p_inq, p_eir, bt_mesh_scan_result_callback);
}

static bool valid_adv_param(const struct bt_le_adv_param *param)
{
    if (!(param->options & BT_LE_ADV_OPT_CONNECTABLE)) {
        /*
         * BT Core 4.2 [Vol 2, Part E, 7.8.5]
         * The Advertising_Interval_Min and Advertising_Interval_Max
         * shall not be set to less than 0x00A0 (100 ms) if the
         * Advertising_Type is set to ADV_SCAN_IND or ADV_NONCONN_IND.
         */
#if BT_DEV
        /**TODO: Need to optimize controller to make it supports 20ms interval */
        if (bt_dev.hci_version < BT_HCI_VERSION_5_0 &&
                param->interval_min < 0x00a0) {
            return false;
        }
#endif
    }

    if (param->interval_min > param->interval_max ||
            param->interval_min < 0x0020 || param->interval_max > 0x4000) {
        return false;
    }

    return true;
}

static int set_ad(u16_t hci_op, const struct bt_data *ad, size_t ad_len)
{
    struct bt_hci_cp_le_set_adv_data set_data_param;
    struct bt_hci_cp_le_set_adv_data *set_data = &set_data_param;
    int i;

#if 0
    struct net_buf *buf;
    buf = bt_hci_cmd_create(hci_op, sizeof(*set_data));
    if (!buf) {
        return -ENOBUFS;
    }
#endif

    memset(set_data, 0, sizeof(*set_data));

    for (i = 0; i < ad_len; i++) {
        /* Check if ad fit in the remaining buffer */
        if (set_data->len + ad[i].data_len + 2 > 31) {
#if 0
            net_buf_unref(buf);
#endif
            return -EINVAL;
        }

        set_data->data[set_data->len++] = ad[i].data_len + 1;
        set_data->data[set_data->len++] = ad[i].type;

        memcpy(&set_data->data[set_data->len], ad[i].data, ad[i].data_len);
        set_data->len += ad[i].data_len;
    }

    /* Set adv data and scan rsp data, TODO: how to process null adv data?
     * Do we need to set adv data every time even though the adv data is not changed?
     */

#if 0
    return bt_hci_cmd_send_sync(hci_op, buf, NULL);
#else
    if (hci_op == BT_HCI_OP_LE_SET_ADV_DATA) {
        BLE_MESH_BTM_CHECK_STATUS(BTM_BleWriteAdvDataRaw(set_data->data, set_data->len));
    } else if (hci_op == BT_HCI_OP_LE_SET_SCAN_RSP_DATA) {
        BLE_MESH_BTM_CHECK_STATUS(BTM_BleWriteScanRspRaw(set_data->data, set_data->len));
    }
    return 0;
#endif /* #if 0 */
}

static void start_adv_completed_cb(u8_t status)
{
    /**TODO: It is too late to wait for completed event*/
#if 0
    if (!status) {
        atomic_set_bit(bt_dev.flags, BT_DEV_ADVERTISING);
    }
#endif
}

static bool valid_le_scan_param(const struct bt_le_scan_param *param)
{
    if (param->type != BT_HCI_LE_SCAN_PASSIVE &&
            param->type != BT_HCI_LE_SCAN_ACTIVE) {
        return false;
    }

    if (param->filter_dup != BT_HCI_LE_SCAN_FILTER_DUP_DISABLE &&
            param->filter_dup != BT_HCI_LE_SCAN_FILTER_DUP_ENABLE) {
        return false;
    }

    if (param->interval < 0x0004 || param->interval > 0x4000) {
        return false;
    }

    if (param->window < 0x0004 || param->window > 0x4000) {
        return false;
    }

    if (param->window > param->interval) {
        return false;
    }

    return true;
}

static int start_le_scan(u8_t scan_type, u16_t interval, u16_t window, u8_t filter_dup)
{
    int err = 0;

#if 0
    struct bt_hci_cp_le_set_scan_param set_param;
    struct net_buf *buf;
    memset(&set_param, 0, sizeof(set_param));

    set_param.scan_type = scan_type;
    /* for the rest parameters apply default values according to
     *  spec 4.2, vol2, part E, 7.8.10
     */
    set_param.interval = sys_cpu_to_le16(interval);
    set_param.window = sys_cpu_to_le16(window);
    set_param.filter_policy = 0x00;
#endif

    /* TODO: Currently support only RPA and set only once before scan */
#if 0
    if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
        err = le_set_private_addr();
        if (err) {
            return err;
        }

        if (BT_FEAT_LE_PRIVACY(bt_dev.le.features)) {
            set_param.addr_type = BT_HCI_OWN_ADDR_RPA_OR_RANDOM;
        } else {
            set_param.addr_type = BT_ADDR_LE_RANDOM;
        }
    } else {
        set_param.addr_type =  bt_dev.id_addr.type;

        /* Use NRPA unless identity has been explicitly requested
         * (through Kconfig), or if there is no advertising ongoing.
         */
        if (!IS_ENABLED(CONFIG_BT_SCAN_WITH_IDENTITY) &&
                scan_type == BT_HCI_LE_SCAN_ACTIVE &&
                !atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING)) {
            err = le_set_private_addr();
            if (err) {
                return err;
            }

            set_param.addr_type = BT_ADDR_LE_RANDOM;
        }
    }
#endif

#if 0
    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_SCAN_PARAM, sizeof(set_param));
    if (!buf) {
        return -ENOBUFS;
    }

    net_buf_add_mem(buf, &set_param, sizeof(set_param));

    bt_hci_cmd_send(BT_HCI_OP_LE_SET_SCAN_PARAM, buf);

    err = set_le_scan_enable(BT_HCI_LE_SCAN_ENABLE);
    if (err) {
        return err;
    }
#else
    tGATT_IF client_if = 0xFF;      //default GATT interface id
    UINT8 scan_fil_policy = 0x00;   //No white-list for mesh
    UINT8 addr_type_own = 0x0;      //TODO: currently support only RPA

    /*TODO: Need to process scan_param_setup_cback
     * Need to add menuconfig for duplicate scan*/
    BTM_BleSetScanFilterParams(client_if, interval, window, scan_type, addr_type_own,
                               filter_dup, scan_fil_policy, NULL);

    /*TODO: Need to process p_start_stop_scan_cb to check if start successfully */
    /* BLE Mesh scan permanently, so no duration of scan here */
    BLE_MESH_BTM_CHECK_STATUS(BTM_BleScan(true, 0, ble_mesh_scan_results_cb, NULL, NULL));
#endif /* #if 0 */

#if BT_DEV
    if (scan_type == BT_HCI_LE_SCAN_ACTIVE) {
        atomic_set_bit(bt_dev.flags, BT_DEV_ACTIVE_SCAN);
    } else {
        atomic_clear_bit(bt_dev.flags, BT_DEV_ACTIVE_SCAN);
    }
#endif

    return err;
}

static void bt_mesh_scan_result_callback(tBTA_DM_SEARCH_EVT event, tBTA_DM_SEARCH *p_data)
{
    bt_addr_le_t addr;
    UINT8 rssi;
    UINT8 adv_type;

    BT_DBG("%s, event = %d", __func__, event);

    if (event == BTA_DM_INQ_RES_EVT) {
        /* TODO: How to process scan_rsp here? */
        addr.type = p_data->inq_res.ble_addr_type;
        memcpy(addr.a.val, p_data->inq_res.bd_addr, BD_ADDR_LEN);
        rssi = p_data->inq_res.rssi;
        adv_type = p_data->inq_res.ble_evt_type;

        //scan rsp len: p_data->inq_res.scan_rsp_len
        struct net_buf_simple *buf = bt_mesh_alloc_buf(p_data->inq_res.adv_data_len);
        if (!buf) {
            BT_ERR("%s, Failed to allocate memory", __func__);
            return;
        }
        net_buf_simple_init(buf, 0);
        net_buf_simple_add_mem(buf, p_data->inq_res.p_eir, p_data->inq_res.adv_data_len);

        if (bt_mesh_scan_dev_found_cb != NULL) {
            bt_mesh_scan_dev_found_cb(&addr, rssi, adv_type, buf);
        }
        osi_free(buf);
    } else if (event == BTA_DM_INQ_CMPL_EVT) {
        BT_INFO("%s, Scan completed, number of scan response %d", __func__, p_data->inq_cmpl.num_resps);
    } else {
        BT_WARN("%s, Unexpected event 0x%x", __func__, event);
    }
}

int bt_le_scan_update(bool fast_scan)
{
#if BT_DEV
    if (atomic_test_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN)) {
        return 0;
    }
#endif /* #if BT_DEV */

#if BT_DEV
    if (atomic_test_bit(bt_dev.flags, BT_DEV_SCANNING)) {
#else /* #if BT_DEV */
    if (1) {
#endif /* #if BT_DEV */
#if 0
        int err = set_le_scan_enable(BT_HCI_LE_SCAN_DISABLE);
        if (err) {
            return err;
        }
#else /* #if 0 */
        /* TODO: need to process stop_scan_cb */
#if BT_DEV
        atomic_clear_bit(bt_dev.flags, BT_DEV_SCANNING);
#endif
        BLE_MESH_BTM_CHECK_STATUS(BTM_BleScan(false, 0, NULL, NULL, NULL));
#endif /* #if 0 */
    }

    /* We don't need to stay pasive scan for central device */
#if 0
    if (IS_ENABLED(CONFIG_BT_CENTRAL)) {
        u16_t interval, window;
        struct bt_conn *conn;

        conn = bt_conn_lookup_state_le(NULL, BT_CONN_CONNECT_SCAN);
        if (!conn) {
            return 0;
        }

        atomic_set_bit(bt_dev.flags, BT_DEV_SCAN_FILTER_DUP);

        bt_mesh_conn_unref(conn);

        if (fast_scan) {
            interval = BT_GAP_SCAN_FAST_INTERVAL;
            window = BT_GAP_SCAN_FAST_WINDOW;
        } else {
            interval = BT_GAP_SCAN_SLOW_INTERVAL_1;
            window = BT_GAP_SCAN_SLOW_WINDOW_1;
        }

        return start_le_scan(BT_HCI_LE_SCAN_PASSIVE, interval, window);
    }
#endif /* #if 0 */

    return 0;
}

/* APIs functions */
int bt_le_adv_start(const struct bt_le_adv_param *param,
                    const struct bt_data *ad, size_t ad_len,
                    const struct bt_data *sd, size_t sd_len)
{
    /** TODO:
     *- Need to support 20ms interval in 4.2
     *- Need to check adv start HCI event
     *- Random address check, currently we set privacy only once in application
     *- Process ADV_OPT_ONE_TIME
     **/
    UINT16 adv_int_min;
    UINT16 adv_int_max;
    tBLE_ADDR_TYPE addr_type_own;
    UINT8 adv_type;
    tBTM_BLE_ADV_CHNL_MAP chnl_map;
    tBTM_BLE_AFP adv_fil_pol;
    tBLE_BD_ADDR p_dir_bda;
    tBTA_START_ADV_CMPL_CBACK *p_start_adv_cb;

    struct bt_hci_cp_le_set_adv_param set_param;
    int err = 0;

    if (!valid_adv_param(param)) {
        return -EINVAL;
    }

#if BT_DEV
#if 0
    /**TODO: We cancel this check temporarily*/
    if (atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING)) {
        return -EALREADY;
    }
#endif /* #if 0 */
#endif /* #if BT_DEV */

    err = set_ad(BT_HCI_OP_LE_SET_ADV_DATA, ad, ad_len);
    if (err) {
        return err;
    }

    /*
     * We need to set SCAN_RSP when enabling advertising type that allows
     * for Scan Requests.
     *
     * If sd was not provided but we enable connectable undirected
     * advertising sd needs to be cleared from values set by previous calls.
     * Clearing sd is done by calling set_ad() with NULL data and zero len.
     * So following condition check is unusual but correct.
     */
    if (sd || (param->options & BT_LE_ADV_OPT_CONNECTABLE)) {
        err = set_ad(BT_HCI_OP_LE_SET_SCAN_RSP_DATA, sd, sd_len);
        if (err) {
            return err;
        }
    }

    memset(&set_param, 0, sizeof(set_param));

    set_param.min_interval = sys_cpu_to_le16(param->interval_min);
    set_param.max_interval = sys_cpu_to_le16(param->interval_max);
    set_param.channel_map  = 0x07;

    /** TODO: Currently we support only RPA address */
#if 0
    if (param->options & BT_LE_ADV_OPT_CONNECTABLE) {
        if (IS_ENABLED(CONFIG_BT_PRIVACY)) {
            err = le_set_private_addr();
            if (err) {
                return err;
            }

            if (BT_FEAT_LE_PRIVACY(bt_dev.le.features)) {
                set_param.own_addr_type =
                    BT_HCI_OWN_ADDR_RPA_OR_RANDOM;
            } else {
                set_param.own_addr_type = BT_ADDR_LE_RANDOM;
            }
        } else {
            /*
             * If Static Random address is used as Identity
             * address we need to restore it before advertising
             * is enabled. Otherwise NRPA used for active scan
             * could be used for advertising.
             */
            if (atomic_test_bit(bt_dev.flags,
                                BT_DEV_ID_STATIC_RANDOM)) {
                set_random_address(&bt_dev.id_addr.a);
            }

            set_param.own_addr_type = bt_dev.id_addr.type;
        }

        set_param.type = BT_LE_ADV_IND;
    } else {
        if (param->own_addr) {
            /* Only NRPA is allowed */
            if (!BT_ADDR_IS_NRPA(param->own_addr)) {
                return -EINVAL;
            }

            err = set_random_address(param->own_addr);
        } else {
            err = le_set_private_addr();
        }

        if (err) {
            return err;
        }

        set_param.own_addr_type = BT_ADDR_LE_RANDOM;

        if (sd) {
            set_param.type = BT_LE_ADV_SCAN_IND;
        } else {
            set_param.type = BT_LE_ADV_NONCONN_IND;
        }
    }
#else /* #if 0 */
    /* TODO: Has been simplified here, currently support only RPA addr */
    set_param.own_addr_type = 0x0; //BT_ADDR_LE_RANDOM
    if (param->options & BT_LE_ADV_OPT_CONNECTABLE) {
        set_param.type = 0x00; //ADV_TYPE_IND;
    } else if (sd != NULL) {
        set_param.type = 0x02; //ADV_TYPE_SCAN_IND;
    } else {
        set_param.type = 0x03; //ADV_TYPE_NONCONN_IND;
    }
#endif /* #if 0 */

#if 0
    buf = bt_hci_cmd_create(BT_HCI_OP_LE_SET_ADV_PARAM, sizeof(set_param));
    if (!buf) {
        return -ENOBUFS;
    }

    net_buf_add_mem(buf, &set_param, sizeof(set_param));

    err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_SET_ADV_PARAM, buf, NULL);
    if (err) {
        return err;
    }

    err = set_advertise_enable(true);
    if (err) {
        return err;
    }
#else /* #if 0 */
    addr_type_own = set_param.own_addr_type;
    /* Set adv parameters */
    adv_int_min = param->interval_min;
    adv_int_max = param->interval_max;
    adv_type = set_param.type;
    chnl_map = BTA_BLE_ADV_CHNL_37 | BTA_BLE_ADV_CHNL_38 | BTA_BLE_ADV_CHNL_39;
    adv_fil_pol = AP_SCAN_CONN_ALL;
    memset(&p_dir_bda, 0, sizeof(p_dir_bda));
    p_start_adv_cb = start_adv_completed_cb;

    /* TODO: We need to add a check function, to make sure adv start successfully and then set bit
     * So currently we call BTM_BleSetAdvParamsStartAdv instead of BTA_DmSetBleAdvParamsAll
     */
#if 1
    /* Check if we can start adv using BTM_BleSetAdvParamsStartAdvCheck */
    BLE_MESH_BTM_CHECK_STATUS(BTM_BleSetAdvParamsAll(adv_int_min, adv_int_max, adv_type,
                              addr_type_own, &p_dir_bda,
                              chnl_map, adv_fil_pol, p_start_adv_cb));
    BLE_MESH_BTM_CHECK_STATUS(BTM_BleStartAdv());
#if BT_DEV
    atomic_set_bit(bt_dev.flags, BT_DEV_ADVERTISING);
#endif /* #if BT_DEV */
#else /* #if 1 */
    status = BTM_BleSetAdvParamsStartAdv (adv_int_min, adv_int_max, adv_type, addr_type_own, &p_dir_bda, chnl_map, adv_fil_pol, p_start_adv_cb);
    if (!status) {
        atomic_set_bit(bt_dev.flags, BT_DEV_ADVERTISING);
    }
#endif /* #if 1 */
#endif /* #if 0 */

    /** TODO: To implement BT_LE_ADV_OPT_ONE_TIME */
#if BT_DEV
    if (!(param->options & BT_LE_ADV_OPT_ONE_TIME)) {
        atomic_set_bit(bt_dev.flags, BT_DEV_KEEP_ADVERTISING);
    }
#endif /* #if BT_DEV */

    return err;
}

int bt_le_adv_stop(void)
{
    int err = 0;
    UINT8 status;

    /* Make sure advertising is not re-enabled later even if it's not
     * currently enabled (i.e. BT_DEV_ADVERTISING is not set).
     */
#if BT_DEV
    atomic_clear_bit(bt_dev.flags, BT_DEV_KEEP_ADVERTISING);

#if 0
    /**TODO: We cancel this check temporarily*/
    if (!atomic_test_bit(bt_dev.flags, BT_DEV_ADVERTISING)) {
        return 0;
    }
#endif /* #if 0 */
#endif /* #if BT_DEV */

#if 0
    err = set_advertise_enable(false);
    if (err) {
        return err;
    }
#else
    /*TODO: We need to add a check function, to make sure adv start successfully and then set bit
     * So currently we use BTM_BleBroadcast instead of BTA_DmBleBroadcast*/
    status = BTM_BleBroadcast(false, NULL);
    if ((status != BTM_SUCCESS) && (status != BTM_CMD_STARTED)) {
        BT_ERR("%s, invalid status %d", __func__, status);
#if BT_DEV
        atomic_clear_bit(bt_dev.flags, BT_DEV_ADVERTISING);
#endif
        return -1;
    }
#endif /* #if 0 */

#if 0
    /* TODO: Currently support RPA only */
    if (!IS_ENABLED(CONFIG_BT_PRIVACY)) {
        /* If active scan is ongoing set NRPA */
        if (atomic_test_bit(bt_dev.flags, BT_DEV_SCANNING) &&
                atomic_test_bit(bt_dev.flags, BT_DEV_ACTIVE_SCAN)) {
            le_set_private_addr();
        }
    }
#endif

    return err;
}

int bt_le_scan_start(const struct bt_le_scan_param *param, bt_le_scan_cb_t cb)
{
    /* TODO:
     * Do we need to use duplicate mode for mesh scan?
     */
    int err = 0;

    /* Check that the parameters have valid values */
    if (!valid_le_scan_param(param)) {
        return -EINVAL;
    }

    /* Return if active scan is already enabled */
#if BT_DEV
    if (atomic_test_and_set_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN)) {
        return -EALREADY;
    }
#endif

    /* TODO: Currently we use bluedroid APIs, which will stop scan automatically
     * while setting parameters.
     */
#if 0
    if (atomic_test_bit(bt_dev.flags, BT_DEV_SCANNING)) {
        err = set_le_scan_enable(BT_HCI_LE_SCAN_DISABLE);
        if (err) {
            atomic_clear_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN);
            return err;
        }
    }
#endif

#if BT_DEV
    if (param->filter_dup) {
        atomic_set_bit(bt_dev.flags, BT_DEV_SCAN_FILTER_DUP);
    } else {
        atomic_clear_bit(bt_dev.flags, BT_DEV_SCAN_FILTER_DUP);
    }
#endif

    err = start_le_scan(param->type, param->interval, param->window, param->filter_dup);
    if (err) {
#if BT_DEV
        atomic_clear_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN);
#endif
        return err;
    }

#if BT_DEV
    atomic_set_bit(bt_dev.flags, BT_DEV_SCANNING);
#endif

    bt_mesh_scan_dev_found_cb = cb;
    return err;
}

int bt_le_scan_stop(void)
{
    /* Return if active scanning is already disabled */
#if BT_DEV
    if (!atomic_test_and_clear_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN)) {
        return -EALREADY;
    }
#endif

    bt_mesh_scan_dev_found_cb = NULL;

    return bt_le_scan_update(false);
}

#if defined(CONFIG_BT_MESH_NODE) && CONFIG_BT_MESH_NODE
static void bt_mesh_bta_gatts_cb(tBTA_GATTS_EVT event, tBTA_GATTS *p_data)
{
    switch (event) {
    case BTA_GATTS_REG_EVT:
        if (p_data->reg_oper.status == BTA_GATT_OK) {
            bt_mesh_gatts_if = p_data->reg_oper.server_if;
        }
        break;
    case BTA_GATTS_READ_EVT: {
        u8_t buf[100] = {0};
        u16_t len = 0;
        tBTA_GATTS_RSP rsp;
        BT_DBG("%s, read: handle = %d", __func__, p_data->req_data.p_data->read_req.handle);
        u8_t index = BT_MESH_GATT_GET_CONN_ID(p_data->req_data.conn_id);
        struct bt_gatt_attr *mesh_attr = bt_mesh_gatts_find_attr_by_handle(p_data->req_data.p_data->read_req.handle);
        if (mesh_attr != NULL && mesh_attr->read != NULL) {
            if ((len = mesh_attr->read(&bt_mesh_gatts_conn[index], mesh_attr, buf, 100,
                                       p_data->req_data.p_data->read_req.offset)) > 0) {
                rsp.attr_value.handle = p_data->req_data.p_data->read_req.handle;
                rsp.attr_value.len = len;
                memcpy(&rsp.attr_value.value[0], buf, len);
                BTA_GATTS_SendRsp(p_data->req_data.conn_id, p_data->req_data.trans_id,
                                  p_data->req_data.status, &rsp);
                BT_DBG("%s, send mesh read rsp, handle = %x", __func__, mesh_attr->handle);
            } else {
                BT_WARN("%s, read perm Invalid", __func__);
            }
        }
        break;
    }
    case BTA_GATTS_WRITE_EVT: {
        u16_t len = 0;
        BT_DBG("%s, write: handle = %d, len = %d, data = %s", __func__, p_data->req_data.p_data->write_req.handle,
               p_data->req_data.p_data->write_req.len,
               bt_hex(p_data->req_data.p_data->write_req.value, p_data->req_data.p_data->write_req.len));
        u8_t index = BT_MESH_GATT_GET_CONN_ID(p_data->req_data.conn_id);
        struct bt_gatt_attr *mesh_attr = bt_mesh_gatts_find_attr_by_handle(p_data->req_data.p_data->write_req.handle);
        if (mesh_attr != NULL && mesh_attr->write != NULL) {
            if ((len = mesh_attr->write(&bt_mesh_gatts_conn[index], mesh_attr,
                                        p_data->req_data.p_data->write_req.value,
                                        p_data->req_data.p_data->write_req.len,
                                        p_data->req_data.p_data->write_req.offset, 0)) > 0) {
                if (p_data->req_data.p_data->write_req.need_rsp) {
                    BTA_GATTS_SendRsp(p_data->req_data.conn_id, p_data->req_data.trans_id,
                                      p_data->req_data.status, NULL);
                    BT_DBG("%s, send mesh write rsp, handle = %x", __func__, mesh_attr->handle);
                }
            }
        }
        break;
    }
    case BTA_GATTS_EXEC_WRITE_EVT:
        break;
    case BTA_GATTS_MTU_EVT:
        break;
    case BTA_GATTS_CONF_EVT:
        break;
    case BTA_GATTS_CREATE_EVT: {
        svc_handle = p_data->create.service_id;
        BT_DBG("%s, svc_handle = %d, future_mesh = %p", __func__, svc_handle, future_mesh);
        if (future_mesh != NULL) {
            future_ready(future_mesh, FUTURE_SUCCESS);
        }
        break;
    }
    case BTA_GATTS_ADD_INCL_SRVC_EVT: {
        svc_handle = p_data->add_result.attr_id;
        if (future_mesh != NULL) {
            future_ready(future_mesh, FUTURE_SUCCESS);
        }
        break;
    }
    case BTA_GATTS_ADD_CHAR_EVT:
        char_handle = p_data->add_result.attr_id;
        if (future_mesh != NULL) {
            future_ready(future_mesh, FUTURE_SUCCESS);
        }
        break;
    case BTA_GATTS_ADD_CHAR_DESCR_EVT:
        char_handle = p_data->add_result.attr_id;
        if (future_mesh != NULL) {
            future_ready(future_mesh, FUTURE_SUCCESS);
        }
        break;
    case BTA_GATTS_DELELTE_EVT:
        break;
    case BTA_GATTS_START_EVT:
        break;
    case BTA_GATTS_STOP_EVT:
        break;
    case BTA_GATTS_CONNECT_EVT: {
        /*Adv disabled*/
        // atomic_clear_bit(bt_dev.flags, BT_DEV_ADVERTISING);
        if (bt_mesh_gatts_conn_cb != NULL && bt_mesh_gatts_conn_cb->connected != NULL) {
            u8_t index = BT_MESH_GATT_GET_CONN_ID(p_data->conn.conn_id);
            if (index < CONFIG_BT_MAX_CONN) {
                bt_mesh_gatts_conn[index].handle = BT_MESH_GATT_GET_CONN_ID(p_data->conn.conn_id);
                (bt_mesh_gatts_conn_cb->connected)(&bt_mesh_gatts_conn[index], 0);
            }
        }
        break;
    }
    case BTA_GATTS_DISCONNECT_EVT: {
        /*Adv disabled*/
        // atomic_clear_bit(bt_dev.flags, BT_DEV_ADVERTISING);
        if (bt_mesh_gatts_conn_cb != NULL && bt_mesh_gatts_conn_cb->disconnected != NULL) {
            u8_t index = BT_MESH_GATT_GET_CONN_ID(p_data->conn.conn_id);
            if (index < CONFIG_BT_MAX_CONN) {
                bt_mesh_gatts_conn[index].handle = BT_MESH_GATT_GET_CONN_ID(p_data->conn.conn_id);
                (bt_mesh_gatts_conn_cb->disconnected)(&bt_mesh_gatts_conn[index], p_data->conn.reason);
            }
        }
        break;
    }
    case BTA_GATTS_CLOSE_EVT:
        break;
    default:
        break;
    }
}

void bt_mesh_gatts_conn_cb_register(struct bt_conn_cb *cb)
{
    bt_mesh_gatts_conn_cb = cb;
}

static struct bt_gatt_attr *bt_mesh_gatts_find_attr_by_handle(u16_t handle)
{
    struct bt_gatt_service *svc;
    struct bt_gatt_attr *attr = NULL;
    SYS_SLIST_FOR_EACH_CONTAINER(&bt_mesh_gatts_db, svc, node) {
        int i;

        for (i = 0; i < svc->attr_count; i++) {
            attr = &svc->attrs[i];
            /* Check the attrs handle is equal to the handle or not */
            if (attr->handle == handle) {
                return attr;
            }
        }
    }

    return NULL;
}

static void bt_mesh_gatts_foreach_attr(u16_t start_handle, u16_t end_handle,
                bt_gatt_attr_func_t func, void *user_data)
{
    struct bt_gatt_service *svc;

    SYS_SLIST_FOR_EACH_CONTAINER(&bt_mesh_gatts_db, svc, node) {
        int i;

        for (i = 0; i < svc->attr_count; i++) {
            struct bt_gatt_attr *attr = &svc->attrs[i];

            /* Check if attribute handle is within range */
            if (attr->handle < start_handle ||
                    attr->handle > end_handle) {
                continue;
            }

            if (func(attr, user_data) == BT_GATT_ITER_STOP) {
                return;
            }
        }
    }
}

static u8_t find_next(const struct bt_gatt_attr *attr, void *user_data)
{
    struct bt_gatt_attr **next = user_data;

    *next = (struct bt_gatt_attr *)attr;

    return BT_GATT_ITER_STOP;
}

static struct bt_gatt_attr *bt_mesh_gatts_attr_next(const struct bt_gatt_attr *attr)
{
    struct bt_gatt_attr *next = NULL;

    bt_mesh_gatts_foreach_attr(attr->handle + 1, attr->handle + 1, find_next, &next);

    return next;
}

ssize_t bt_mesh_gatts_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, u16_t buf_len, u16_t offset,
                                const void *value, u16_t value_len)
{
    u16_t len;

    if (offset > value_len) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    len = min(buf_len, value_len - offset);

    BT_DBG("handle 0x%04x offset %u length %u", attr->handle, offset, len);

    memcpy(buf, value + offset, len);

    return len;
}

struct gatts_incl {
    u16_t start_handle;
    u16_t end_handle;
    u16_t uuid16;
} __packed;

ssize_t bt_mesh_gatts_attr_read_included(struct bt_conn *conn,
                                         const struct bt_gatt_attr *attr,
                                         void *buf, u16_t len, u16_t offset)
{
    struct bt_gatt_attr *incl = attr->user_data;
    struct bt_uuid *uuid = incl->user_data;
    struct gatts_incl pdu;
    u8_t value_len;

    /* first attr points to the start handle */
    pdu.start_handle = sys_cpu_to_le16(incl->handle);
    value_len = sizeof(pdu.start_handle) + sizeof(pdu.end_handle);

    /*
     * Core 4.2, Vol 3, Part G, 3.2,
     * The Service UUID shall only be present when the UUID is a
     * 16-bit Bluetooth UUID.
     */
    if (uuid->type == BT_UUID_TYPE_16) {
        pdu.uuid16 = sys_cpu_to_le16(BT_UUID_16(uuid)->val);
        value_len += sizeof(pdu.uuid16);
    }

    /* Lookup for service end handle */
    //bt_mesh_gatts_foreach_attr(incl->handle + 1, 0xffff, get_service_handles, &pdu);

    return bt_mesh_gatts_attr_read(conn, attr, buf, len, offset, &pdu, value_len);
}

ssize_t bt_mesh_gatts_attr_read_service(struct bt_conn *conn,
                                        const struct bt_gatt_attr *attr,
                                        void *buf, u16_t len, u16_t offset)
{
    struct bt_uuid *uuid = attr->user_data;

    if (uuid->type == BT_UUID_TYPE_16) {
        u16_t uuid16 = sys_cpu_to_le16(BT_UUID_16(uuid)->val);

        return bt_mesh_gatts_attr_read(conn, attr, buf, len, offset, &uuid16, 2);
    }

    return bt_mesh_gatts_attr_read(conn, attr, buf, len, offset,
                             BT_UUID_128(uuid)->val, 16);
}

struct gatts_chrc {
    u8_t properties;
    u16_t value_handle;
    union {
        u16_t uuid16;
        u8_t  uuid[16];
    };
} __packed;

ssize_t bt_mesh_gatts_attr_read_chrc(struct bt_conn *conn,
                                     const struct bt_gatt_attr *attr, void *buf,
                                     u16_t len, u16_t offset)
{
    struct bt_gatt_chrc *chrc = attr->user_data;
    struct gatts_chrc pdu;
    const struct bt_gatt_attr *next;
    u8_t value_len;

    pdu.properties = chrc->properties;
    /* BLUETOOTH SPECIFICATION Version 4.2 [Vol 3, Part G] page 534:
     * 3.3.2 Characteristic Value Declaration
     * The Characteristic Value declaration contains the value of the
     * characteristic. It is the first Attribute after the characteristic
     * declaration. All characteristic definitions shall have a
     * Characteristic Value declaration.
     */
    next = bt_mesh_gatts_attr_next(attr);
    if (!next) {
        BT_WARN("No value for characteristic at 0x%04x", attr->handle);
        pdu.value_handle = 0x0000;
    } else {
        pdu.value_handle = sys_cpu_to_le16(next->handle);
    }
    value_len = sizeof(pdu.properties) + sizeof(pdu.value_handle);

    if (chrc->uuid->type == BT_UUID_TYPE_16) {
        pdu.uuid16 = sys_cpu_to_le16(BT_UUID_16(chrc->uuid)->val);
        value_len += 2;
    } else {
        memcpy(pdu.uuid, BT_UUID_128(chrc->uuid)->val, 16);
        value_len += 16;
    }

    return bt_mesh_gatts_attr_read(conn, attr, buf, len, offset, &pdu, value_len);
}

static void bta_uuid_to_mesh_uuid(tBT_UUID *bta_uuid, const struct bt_uuid  *uuid)
{
    assert(uuid != NULL && bta_uuid != NULL);

    if (uuid->type == BT_UUID_TYPE_16) {
        bta_uuid->len = LEN_UUID_16;
        bta_uuid->uu.uuid16 = BT_UUID_16(uuid)->val;
    } else if (uuid->type == BT_UUID_TYPE_32) {
        bta_uuid->len = LEN_UUID_32;
        bta_uuid->uu.uuid32 = BT_UUID_32(uuid)->val;
    } else if (uuid->type == BT_UUID_TYPE_128) {
        bta_uuid->len = LEN_UUID_128;
        memcpy(bta_uuid->uu.uuid128, BT_UUID_128(uuid)->val, LEN_UUID_128);
    } else {
        BT_ERR("%s, Invalid mesh uuid type = %d", __func__, uuid->type);
    }

    return;
}

static int gatts_register(struct bt_gatt_service *svc)
{
    struct bt_gatt_service *last;
    u16_t handle;
    //struct bt_gatt_attr *attrs = svc->attrs;
    //u16_t count = svc->attr_count;

    if (sys_slist_is_empty(&bt_mesh_gatts_db)) {
        handle = 0;
        goto populate;
    }

    last = SYS_SLIST_PEEK_TAIL_CONTAINER(&bt_mesh_gatts_db, last, node);
    handle = last->attrs[last->attr_count - 1].handle;
    BT_DBG("%s, handle =  %d", __func__, handle);

populate:
#if 0
    /* Populate the handles and append them to the list */
    for (; attrs && count; attrs++, count--) {
        if (!attrs->handle) {
            /* Allocate handle if not set already */
            attrs->handle = ++handle;
        } else if (attrs->handle > handle) {
            /* Use existing handle if valid */
            handle = attrs->handle;
        } else {
            /* Service has conflicting handles */
            BT_ERR("Unable to register handle 0x%04x",
                   attrs->handle);
            return -EINVAL;
        }

        BT_DBG("attr %p handle 0x%04x uuid %s perm 0x%02x",
               attrs, attrs->handle, bt_uuid_str(attrs->uuid),
               attrs->perm);
    }
#endif
    sys_slist_append(&bt_mesh_gatts_db, &svc->node);
    return 0;
}

static tBTA_GATT_PERM mesh_perm_to_bta_perm(u8_t perm)
{
    tBTA_GATT_PERM bta_perm = 0;
    if ((perm & BT_GATT_PERM_READ) == BT_GATT_PERM_READ) {
        bta_perm |= BTA_GATT_PERM_READ;
    }

    if ((perm & BT_GATT_PERM_WRITE) == BT_GATT_PERM_WRITE) {
        bta_perm |= BTA_GATT_PERM_WRITE;
    }

    if ((perm & BT_GATT_PERM_READ_ENCRYPT) ==  BT_GATT_PERM_READ_ENCRYPT) {
        bta_perm |= BTA_GATT_PERM_READ_ENCRYPTED;
    }

    if ((perm & BT_GATT_PERM_WRITE_ENCRYPT) == BT_GATT_PERM_WRITE_ENCRYPT) {
        bta_perm |= BTA_GATT_PERM_WRITE_ENCRYPTED;
    }

    if ((perm & BT_GATT_PERM_READ_AUTHEN) == BT_GATT_PERM_READ_AUTHEN) {
        bta_perm |= BTA_GATT_PERM_READ_ENC_MITM;
    }

    if ((perm & BT_GATT_PERM_WRITE_AUTHEN) == BT_GATT_PERM_WRITE_AUTHEN) {
        bta_perm |= BTA_GATT_PERM_WRITE_ENC_MITM;
    }

    return bta_perm;
}

int bt_mesh_gatts_service_register(struct bt_gatt_service *svc)
{
    assert(svc != NULL);
    tBT_UUID bta_uuid = {0};

    for (int i = 0; i < svc->attr_count; i++) {
        if (svc->attrs[i].uuid->type == BT_UUID_TYPE_16) {
            switch (BT_UUID_16(svc->attrs[i].uuid)->val) {
            case BT_UUID_GATT_PRIMARY_VAL: {
                future_mesh = future_new();
                bta_uuid_to_mesh_uuid(&bta_uuid, (struct bt_uuid *)svc->attrs[i].user_data);
                BTA_GATTS_CreateService(bt_mesh_gatts_if,
                                        &bta_uuid, 0, svc->attr_count, true);
                if (future_await(future_mesh) == FUTURE_FAIL) {
                    BT_ERR("add primary service failed.");
                    return ESP_FAIL;
                }
                svc->attrs[i].handle = svc_handle;
                BT_DBG("############## create service ############");
                BT_DBG("add pri service: svc_uuid = %x, perm = %d, svc_handle = %d", bta_uuid.uu.uuid16, svc->attrs[i].perm, svc_handle);
                break;
            }
            case BT_UUID_GATT_SECONDARY_VAL: {
                future_mesh = future_new();
                bta_uuid_to_mesh_uuid(&bta_uuid, (struct bt_uuid *)svc->attrs[i].user_data);
                BTA_GATTS_CreateService(bt_mesh_gatts_if,
                                        &bta_uuid, 0, svc->attr_count, false);
                if (future_await(future_mesh) == FUTURE_FAIL) {
                    BT_ERR("add secondary service failed.");
                    return ESP_FAIL;
                }
                svc->attrs[i].handle = svc_handle;
                BT_DBG("add sec service: svc_uuid = %x, perm = %d, svc_handle = %d", bta_uuid.uu.uuid16, svc->attrs[i].perm, svc_handle);
                break;
            }
            case BT_UUID_GATT_INCLUDE_VAL: {
                break;
            }
            case BT_UUID_GATT_CHRC_VAL: {
                future_mesh = future_new();
                struct bt_gatt_chrc *gatts_chrc = (struct bt_gatt_chrc *)svc->attrs[i].user_data;
                bta_uuid_to_mesh_uuid(&bta_uuid, gatts_chrc->uuid);
                BTA_GATTS_AddCharacteristic(svc_handle, &bta_uuid, mesh_perm_to_bta_perm(svc->attrs[i + 1].perm), gatts_chrc->properties, NULL, NULL);
                if (future_await(future_mesh) == FUTURE_FAIL) {
                    BT_ERR("Add characristic failed.");
                    return ESP_FAIL;
                }
                /* All the characristic should have two handle: the declaration handle and the value handle */
                svc->attrs[i].handle = char_handle - 1;
                svc->attrs[i + 1].handle =  char_handle;
                BT_DBG("add char: char_uuid = %x, char_handle = %d, perm = %d, char_pro = %d", BT_UUID_16(gatts_chrc->uuid)->val, char_handle, svc->attrs[i + 1].perm, gatts_chrc->properties);
                break;
            }
            case BT_UUID_GATT_CEP_VAL:
            case BT_UUID_GATT_CUD_VAL:
            case BT_UUID_GATT_CCC_VAL:
            case BT_UUID_GATT_SCC_VAL:
            case BT_UUID_GATT_CPF_VAL:
            case BT_UUID_VALID_RANGE_VAL:
            case BT_UUID_HIDS_EXT_REPORT_VAL:
            case BT_UUID_HIDS_REPORT_REF_VAL:
            case BT_UUID_ES_CONFIGURATION_VAL:
            case BT_UUID_ES_MEASUREMENT_VAL:
            case BT_UUID_ES_TRIGGER_SETTING_VAL: {
                future_mesh = future_new();
                bta_uuid_to_mesh_uuid(&bta_uuid, svc->attrs[i].uuid);
                BTA_GATTS_AddCharDescriptor(svc_handle, mesh_perm_to_bta_perm(svc->attrs[i].perm), &bta_uuid, NULL, NULL);
                if (future_await(future_mesh) == FUTURE_FAIL) {
                    BT_ERR("add primary service failed.");
                    return ESP_FAIL;
                }
                svc->attrs[i].handle = char_handle;
                BT_DBG("add descr: descr_uuid = %x, perm= %d, descr_handle = %d", BT_UUID_16(svc->attrs[i].uuid)->val, svc->attrs[i].perm, char_handle);
                break;
            }
            default:
                break;
            }
        }
    }

    if (svc_handle != 0) {
        /* TODO: Currently we start service according to function like
         * bt_mesh_proxy_gatt_enable, etc
         */
        //BTA_GATTS_StartService(svc_handle, BTA_GATT_TRANSPORT_LE);
        svc_handle = 0;
    }

    // Still should regitster to the adapt bt_mesh_gatts_db.
    gatts_register(svc);
    return 0;
}

int bt_mesh_gatts_disconnect(struct bt_conn *conn, u8_t reason)
{
    UNUSED(reason);
    u16_t conn_id = BT_MESH_GATT_CREATE_CONN_ID(bt_mesh_gatts_if, conn->handle);
    BTA_GATTS_Close(conn_id);
    return 0;
}

int bt_mesh_gatts_service_unregister(struct bt_gatt_service *svc)
{
    assert(svc != NULL);

    BTA_GATTS_DeleteService(svc->attrs[0].handle);
    return 0;
}

int bt_mesh_gatts_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *data, u16_t len)
{
    u16_t conn_id = BT_MESH_GATT_CREATE_CONN_ID(bt_mesh_gatts_if, conn->handle);
    BTA_GATTS_HandleValueIndication(conn_id, attr->handle, len, (u8_t *)data, false);
    return 0;
}

u16_t bt_mesh_gatt_get_mtu(struct bt_conn *conn)
{
    return BTA_GATT_GetLocalMTU();
}

/* APIs added by Espressif */
int bt_mesh_gatts_service_stop(struct bt_gatt_service *svc)
{
    if (!svc) {
        BT_ERR("%s, svc should not be NULL", __func__);
        return -EINVAL;
    }

    BT_DBG("Stop service:%d", svc->attrs[0].handle);

    BTA_GATTS_StopService(svc->attrs[0].handle);
    return 0;
}

int bt_mesh_gatts_service_start(struct bt_gatt_service *svc)
{
    if (!svc) {
        BT_ERR("%s, svc should not be NULL", __func__);
        return -EINVAL;
    }

    BT_DBG("Start service:%d", svc->attrs[0].handle);

    BTA_GATTS_StartService(svc->attrs[0].handle, BTA_GATT_TRANSPORT_LE);
    return 0;
}
#endif /* defined(CONFIG_BT_MESH_NODE) && CONFIG_BT_MESH_NODE */

#if defined(CONFIG_BT_MESH_PROVISIONER) && CONFIG_BT_MESH_PROVISIONER
void bt_mesh_gattc_conn_cb_register(struct bt_prov_conn_cb *cb)
{
    bt_mesh_gattc_conn_cb = cb;
}

u16_t bt_mesh_gattc_get_service_uuid(struct bt_conn *conn)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
        if (conn == &bt_mesh_gattc_info[i].conn) {
            break;
        }
    }

    if (i == ARRAY_SIZE(bt_mesh_gattc_info)) {
        return 0;
    }

    return bt_mesh_gattc_info[i].service_uuid;
}

/** For provisioner acting as a GATT client, it may follow the procedures
 *  listed below.
 *  1. Create connection with the unprovisioned device
 *  2. Exchange MTU size
 *  3. Find Mesh Prov Service in the device's service database
 *  4. Find Mesh Prov Data In/Out characteristic within the service
 *  5. Get CCC of Mesh Prov Data Out Characteristic
 *  6. Set the Notification bit of CCC
 */

int bt_mesh_gattc_conn_create(const bt_addr_le_t *addr, u16_t service_uuid)
{
    u8_t zero[6] = {0};
    int i;

    if (!addr || !memcmp(addr->a.val, zero, BD_ADDR_LEN) ||
        (addr->type > BLE_ADDR_RANDOM)) {
        BT_ERR("%s: invalid address", __func__);
        return -EINVAL;
    }

    if (service_uuid != BT_UUID_MESH_PROV_VAL &&
        service_uuid != BT_UUID_MESH_PROXY_VAL) {
        BT_ERR("%s: invalid service uuid 0x%04x", __func__, service_uuid);
        return -EINVAL;
    }

    /* Check if already creating connection with the device */
    for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
        if (!memcmp(bt_mesh_gattc_info[i].addr, addr->a.val, BD_ADDR_LEN)) {
            BT_WARN("%s, Already create conn with %s",
                __func__, bt_hex(addr->a.val, BD_ADDR_LEN));
            return -EALREADY;
        }
    }

    /* Find empty element in queue to store device info */
    for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
        if ((bt_mesh_gattc_info[i].conn.handle == 0xFFFF) &&
            (bt_mesh_gattc_info[i].service_uuid == 0x0000)) {
            memcpy(bt_mesh_gattc_info[i].addr, addr->a.val, BD_ADDR_LEN);
            bt_mesh_gattc_info[i].addr_type = addr->type;
            /* Service to be found after exhanging mtu size */
            bt_mesh_gattc_info[i].service_uuid = service_uuid;
            break;
        }
    }

    if (i == ARRAY_SIZE(bt_mesh_gattc_info)) {
        BT_WARN("%s, gattc info is full", __func__);
        return -ENOMEM;
    }

#if BT_DEV
    if (atomic_test_and_clear_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN)) {
        BTM_BleScan(false, 0, NULL, NULL, NULL);
    }
#else
    BTM_BleScan(false, 0, NULL, NULL, NULL);
#endif /* BT_DEV */

    BT_DBG("%s, create conn with %s", __func__, bt_hex(addr->a.val, BD_ADDR_LEN));

    /* Min_interval: 250ms
     * Max_interval: 250ms
     * Slave_latency: 0x0
     * Supervision_timeout: 32 sec
     */
    BTA_DmSetBlePrefConnParams(bt_mesh_gattc_info[i].addr, 0xC8, 0xC8, 0x00, 0xC80);

    BTA_GATTC_Open(bt_mesh_gattc_if, bt_mesh_gattc_info[i].addr,
        bt_mesh_gattc_info[i].addr_type, true, BTA_GATT_TRANSPORT_LE);

    /* Increment pbg_count */
    provisioner_pbg_count_inc();

    return 0;
}

void bt_mesh_gattc_exchange_mtu(u8_t index)
{
    /** Set local MTU and exchange with GATT server.
     *  ATT_MTU >= 69 for Mesh GATT Prov Service
     *  ATT_NTU >= 33 for Mesh GATT Proxy Service
    */
    u16_t conn_id;

    conn_id = BT_MESH_GATT_CREATE_CONN_ID(bt_mesh_gattc_if, bt_mesh_gattc_info[index].conn.handle);

    BTA_GATTC_ConfigureMTU(conn_id);
}

u16_t bt_mesh_gattc_get_mtu_info(struct bt_conn *conn)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
        if (conn == &bt_mesh_gattc_info[i].conn) {
            return bt_mesh_gattc_info[i].mtu;
        }
    }

    return 0;
}

int bt_mesh_gattc_write_no_rsp(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *data, u16_t len)
{
    int i;
    u16_t conn_id;

    for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
        if (conn == &bt_mesh_gattc_info[i].conn) {
            break;
        }
    }

    if (i == ARRAY_SIZE(bt_mesh_gattc_info)) {
        BT_ERR("No conn found");
        /** Here we return 0 for prov_send() return value check in provisioner.c
         */
        return 0;
    }

    conn_id = BT_MESH_GATT_CREATE_CONN_ID(bt_mesh_gattc_if, bt_mesh_gattc_info[i].conn.handle);

    BTA_GATTC_WriteCharValue(conn_id, bt_mesh_gattc_info[i].data_in_handle,
                             BTA_GATTC_TYPE_WRITE_NO_RSP, len,
                             (u8_t *)data, BTA_GATT_AUTH_REQ_NONE);

    return 0;
}

void bt_mesh_gattc_disconnect(struct bt_conn *conn)
{
    /** Disconnect
     *  Clear proper proxy server information
     *  Clear proper prov_link information
     *  Clear proper bt_mesh_gattc_info information
     *  Here in adapter, we just clear proper bt_mesh_gattc_info, and
     *  when proxy_disconnected callback comes, the proxy server
     *  information and prov_link information should be cleared.
     */
    int i;
    u16_t conn_id;

    for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
        if (conn == &bt_mesh_gattc_info[i].conn) {
            break;
        }
    }

    if (i == ARRAY_SIZE(bt_mesh_gattc_info)) {
        BT_ERR("No conn found");
        return;
    }

    conn_id = BT_MESH_GATT_CREATE_CONN_ID(bt_mesh_gattc_if, bt_mesh_gattc_info[i].conn.handle);

    BTA_GATTC_Close(conn_id);
}

/** Mesh Provisioning Service:  0x1827
 *  Mesh Provisioning Data In:  0x2ADB
 *  Mesh Provisioning Data Out: 0x2ADC
 *  Mesh Proxy Service:  0x1828
 *  Mesh Proxy Data In:  0x2ADD
 *  Mesh PROXY Data Out: 0x2ADE
 */
static void bt_mesh_bta_gattc_cb(tBTA_GATTC_EVT event, tBTA_GATTC *p_data)
{
    struct bt_conn *conn = NULL;
    u16_t handle = 0;
    ssize_t len = 0;
    int i = 0;

    switch (event) {
    case BTA_GATTC_REG_EVT:
        if (p_data->reg_oper.status == BTA_GATT_OK) {
            u8_t uuid[16] = { [0 ... 15] = BT_MESH_GATTC_APP_UUID_BYTE };

            BT_DBG("BTA_GATTC_REG_EVT");

            if (p_data->reg_oper.app_uuid.len == LEN_UUID_128 &&
                    !memcmp(p_data->reg_oper.app_uuid.uu.uuid128, uuid, 16)) {
                bt_mesh_gattc_if = p_data->reg_oper.client_if;
                BT_DBG("bt_mesh_gattc_if is %d", bt_mesh_gattc_if);
            }
        }
        break;
    case BTA_GATTC_CFG_MTU_EVT: {
        if (p_data->cfg_mtu.status == BTA_GATT_OK) {
            BT_DBG("BTA_GATTC_CFG_MTU_EVT, cfg_mtu is %d", p_data->cfg_mtu.mtu);

            handle = BT_MESH_GATT_GET_CONN_ID(p_data->cfg_mtu.conn_id);

            for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
                if (bt_mesh_gattc_info[i].conn.handle == handle) {
                    bt_mesh_gattc_info[i].mtu = p_data->cfg_mtu.mtu;
                    break;
                }
            }

            /** Once mtu exchanged accomplished, start to find services, and here
             *  need a flag to indicate which service to find(Mesh Prov Service or
             *  Mesh Proxy Service)
             */
            if (i != ARRAY_SIZE(bt_mesh_gattc_info)) {
                tBT_UUID service_uuid;
                u16_t conn_id;

                conn_id = BT_MESH_GATT_CREATE_CONN_ID(bt_mesh_gattc_if, bt_mesh_gattc_info[i].conn.handle);
                service_uuid.len       = sizeof(bt_mesh_gattc_info[i].service_uuid);
                service_uuid.uu.uuid16 = bt_mesh_gattc_info[i].service_uuid;

                /* Search Mesh Provisioning Service or Mesh Proxy Service */
                BTA_GATTC_ServiceSearchRequest(conn_id, &service_uuid);
            }
        }
        break;
    }
    case BTA_GATTC_SEARCH_RES_EVT: {
        BT_DBG("BTA_GATTC_SEARCH_RES_EVT");

        handle = BT_MESH_GATT_GET_CONN_ID(p_data->srvc_res.conn_id);

        for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
            if (bt_mesh_gattc_info[i].conn.handle == handle) {
                break;
            }
        }

        if (i != ARRAY_SIZE(bt_mesh_gattc_info)) {
            if (p_data->srvc_res.service_uuid.uuid.len == 2 &&
                    p_data->srvc_res.service_uuid.uuid.uu.uuid16 == bt_mesh_gattc_info[i].service_uuid) {
                bt_mesh_gattc_info[i].start_handle = p_data->srvc_res.start_handle;
                bt_mesh_gattc_info[i].end_handle   = p_data->srvc_res.end_handle;
            }
        }
        break;
    }
    case BTA_GATTC_SEARCH_CMPL_EVT: {
        if (p_data->search_cmpl.status == BTA_GATT_OK) {
            BT_DBG("BTA_GATTC_SEARCH_CMPL_EVT");

            handle = BT_MESH_GATT_GET_CONN_ID(p_data->search_cmpl.conn_id);

            for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
                if (bt_mesh_gattc_info[i].conn.handle == handle) {
                    break;
                }
            }

            if (i == ARRAY_SIZE(bt_mesh_gattc_info)) {
                BT_ERR("BTA_GATTC_SEARCH_CMPL_EVT: conn_id not found");
                return;
            }

            conn = &bt_mesh_gattc_info[i].conn;

            if (bt_mesh_gattc_info[i].start_handle == 0x00 ||
                    bt_mesh_gattc_info[i].end_handle   == 0x00 ||
                    (bt_mesh_gattc_info[i].start_handle > bt_mesh_gattc_info[i].end_handle)) {
                bt_mesh_gattc_disconnect(conn);
                return;
            }

            int count = 0;
            int num = 0;
            u16_t conn_id;
            tBT_UUID char_uuid;
            btgatt_db_element_t *result = NULL;
            tBTA_GATT_STATUS status;
            u16_t notify_en = BT_GATT_CCC_NOTIFY;
            tBTA_GATT_UNFMT write;

            /* Get the characteristic num within Mesh Provisioning/Proxy Service */
            conn_id = BT_MESH_GATT_CREATE_CONN_ID(bt_mesh_gattc_if, bt_mesh_gattc_info[i].conn.handle);
            BTA_GATTC_GetDBSizeByType(conn_id, BTGATT_DB_CHARACTERISTIC, bt_mesh_gattc_info[i].start_handle,
                                      bt_mesh_gattc_info[i].end_handle, BTA_GATTC_INVALID_HANDLE, &count);
            if (count != 2) {
                bt_mesh_gattc_disconnect(conn);
                return;
            }

            /* Get Mesh Provisioning/Proxy Data In/Out Characteristic */
            for (int j = 0; j != 2; j++) {
                /** First:  find Mesh Provisioning/Proxy Data In Characteristic
                 *  Second: find Mesh Provisioning/Proxy Data Out Characteristic
                 */
                char_uuid.len = 2;
                if (bt_mesh_gattc_info[i].service_uuid == BT_UUID_MESH_PROV_VAL) {
                    char_uuid.uu.uuid16 = BT_UUID_MESH_PROV_DATA_IN_VAL + j;
                } else if (bt_mesh_gattc_info[i].service_uuid == BT_UUID_MESH_PROXY_VAL) {
                    char_uuid.uu.uuid16 = BT_UUID_MESH_PROXY_DATA_IN_VAL + j;
                }

                BTA_GATTC_GetCharByUUID(conn_id, bt_mesh_gattc_info[i].start_handle,
                                        bt_mesh_gattc_info[i].end_handle, char_uuid, &result, &num);

                if (!result) {
                    bt_mesh_gattc_disconnect(conn);
                    return;
                }

                if (num != 1) {
                    osi_free(result);
                    bt_mesh_gattc_disconnect(conn);
                    return;
                }

                if (!j) {
                    if (!(result[0].properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP)) {
                        osi_free(result);
                        bt_mesh_gattc_disconnect(conn);
                        return;
                    }
                    bt_mesh_gattc_info[i].data_in_handle = result[0].attribute_handle;
                } else {
                    if (!(result[0].properties & BT_GATT_CHRC_NOTIFY)) {
                        osi_free(result);
                        bt_mesh_gattc_disconnect(conn);
                        return;
                    }
                    bt_mesh_gattc_info[i].data_out_handle = result[0].attribute_handle;
                }
                osi_free(result);
                result = NULL;
            }

            /* Register Notification fot Mesh Provisioning/Proxy Data Out Characteristic */
            status = BTA_GATTC_RegisterForNotifications(bt_mesh_gattc_if, bt_mesh_gattc_info[i].addr,
                     bt_mesh_gattc_info[i].data_out_handle);
            if (status != BTA_GATT_OK) {
                bt_mesh_gattc_disconnect(conn);
                return;
            }

            /** After notification is registered, get descriptor number of the
             *  Mesh Provisioning/Proxy Data Out Characteristic
             */
            BTA_GATTC_GetDBSizeByType(conn_id, BTGATT_DB_DESCRIPTOR, bt_mesh_gattc_info[i].start_handle,
                                      bt_mesh_gattc_info[i].end_handle, bt_mesh_gattc_info[i].data_out_handle, &num);
            if (!num) {
                bt_mesh_gattc_disconnect(conn);
                return;
            }

            /* Get CCC of Mesh Provisioning/Proxy Data Out Characteristic */
            char_uuid.len = 2;
            char_uuid.uu.uuid16 = BT_UUID_GATT_CCC_VAL;
            BTA_GATTC_GetDescrByCharHandle(conn_id, bt_mesh_gattc_info[i].data_out_handle,
                                           char_uuid, &result, &num);

            if (!result) {
                bt_mesh_gattc_disconnect(conn);
                return;
            }

            if (num != 1) {
                osi_free(result);
                bt_mesh_gattc_disconnect(conn);
                return;
            }

            bt_mesh_gattc_info[i].ccc_handle = result[0].attribute_handle;

            /** Enable Notification of Mesh Provisioning/Proxy Data Out
             *  Characteristic Descriptor.
             */
            write.len = sizeof(notify_en);
            write.p_value = (u8_t *)&notify_en;
            BTA_GATTC_WriteCharDescr(conn_id, result[0].attribute_handle,
                                     BTA_GATTC_TYPE_WRITE, &write, BTA_GATT_AUTH_REQ_NONE);

            osi_free(result);
            result = NULL;
        }
        break;
    }
    case BTA_GATTC_READ_DESCR_EVT:
        break;
    case BTA_GATTC_WRITE_DESCR_EVT: {
        if (p_data->write.status == BTA_GATT_OK) {
            BT_DBG("BTA_GATTC_WRITE_DESCR_EVT");

            handle = BT_MESH_GATT_GET_CONN_ID(p_data->write.conn_id);

            for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
                if (bt_mesh_gattc_info[i].conn.handle == handle) {
                    break;
                }
            }

            if (i == ARRAY_SIZE(bt_mesh_gattc_info)) {
                BT_ERR("BTA_GATTC_WRITE_DESCR_EVT: conn_id not found");
                return;
            }

            conn = &bt_mesh_gattc_info[i].conn;

            if (bt_mesh_gattc_info[i].ccc_handle != p_data->write.handle) {
                BT_WARN("BTA_GATTC_WRITE_DESCR_EVT: ccc_handle not match");
                bt_mesh_gattc_disconnect(conn);
                return;
            }

            if (bt_mesh_gattc_info[i].service_uuid == BT_UUID_MESH_PROV_VAL) {
                if (bt_mesh_gattc_conn_cb != NULL && bt_mesh_gattc_conn_cb->prov_write_descr != NULL) {
                    len = bt_mesh_gattc_conn_cb->prov_write_descr(&bt_mesh_gattc_info[i].conn, bt_mesh_gattc_info[i].addr);
                    if (len < 0) {
                        BT_ERR("prov_write_descr fail");
                        bt_mesh_gattc_disconnect(conn);
                        return;
                    }
                    bt_mesh_gattc_info[i].wr_desc_done = true;
                }
            } else if (bt_mesh_gattc_info[i].service_uuid == BT_UUID_MESH_PROXY_VAL) {
                if (bt_mesh_gattc_conn_cb != NULL && bt_mesh_gattc_conn_cb->proxy_write_descr != NULL) {
                    len = bt_mesh_gattc_conn_cb->proxy_write_descr(&bt_mesh_gattc_info[i].conn);
                    if (len < 0) {
                        BT_ERR("proxy_write_descr fail");
                        bt_mesh_gattc_disconnect(conn);
                        return;
                    }
                }
            }
        }
        break;
    }
    case BTA_GATTC_NOTIF_EVT: {
        BT_DBG("BTA_GATTC_NOTIF_EVT");

        handle = BT_MESH_GATT_GET_CONN_ID(p_data->notify.conn_id);

        for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
            if (bt_mesh_gattc_info[i].conn.handle == handle) {
                break;
            }
        }

        if (i == ARRAY_SIZE(bt_mesh_gattc_info)) {
            BT_ERR("BTA_GATTC_WRITE_DESCR_EVT: conn_id not found");
            return;
        }

        conn = &bt_mesh_gattc_info[i].conn;

        if (memcmp(bt_mesh_gattc_info[i].addr, p_data->notify.bda, BD_ADDR_LEN) ||
                bt_mesh_gattc_info[i].data_out_handle != p_data->notify.handle ||
                p_data->notify.is_notify == false) {
            BT_ERR("Notification error");
            bt_mesh_gattc_disconnect(conn);
            return;
        }

        if (bt_mesh_gattc_info[i].service_uuid == BT_UUID_MESH_PROV_VAL) {
            if (bt_mesh_gattc_conn_cb != NULL && bt_mesh_gattc_conn_cb->prov_notify != NULL) {
                len = bt_mesh_gattc_conn_cb->prov_notify(&bt_mesh_gattc_info[i].conn,
                                                     p_data->notify.value, p_data->notify.len);
                if (len < 0) {
                    BT_ERR("Receive prov_notify fail");
                    bt_mesh_gattc_disconnect(conn);
                    return;
                }
            }
        } else if (bt_mesh_gattc_info[i].service_uuid == BT_UUID_MESH_PROXY_VAL) {
            if (bt_mesh_gattc_conn_cb != NULL && bt_mesh_gattc_conn_cb->proxy_notify != NULL) {
                len = bt_mesh_gattc_conn_cb->proxy_notify(&bt_mesh_gattc_info[i].conn,
                                                      p_data->notify.value, p_data->notify.len);
                if (len < 0) {
                    BT_ERR("Receive proxy_notify fail");
                    bt_mesh_gattc_disconnect(conn);
                    return;
                }
            }
        }
        break;
    }
    case BTA_GATTC_READ_CHAR_EVT:
        break;
    case BTA_GATTC_WRITE_CHAR_EVT:
        break;
    case BTA_GATTC_PREP_WRITE_EVT:
        break;
    case BTA_GATTC_EXEC_EVT:
        break;
    case BTA_GATTC_OPEN_EVT: {
        BT_DBG("BTA_GATTC_OPEN_EVT");
        /** After current connection is established, provisioner can
         *  use BTA_DmBleScan() to re-enable scan.
         */
        tBTM_STATUS status;
#if BT_DEV
        if (!atomic_test_and_set_bit(bt_dev.flags, BT_DEV_EXPLICIT_SCAN)) {
            status = BTM_BleScan(true, 0, ble_mesh_scan_results_cb, NULL, NULL);
            if (status != BTM_SUCCESS && status != BTM_CMD_STARTED) {
                BT_ERR("%s, invalid status %d", __func__, status);
                break;
            }
        }
#else
        status = BTM_BleScan(true, 0, ble_mesh_scan_results_cb, NULL, NULL);
        if (status != BTM_SUCCESS && status != BTM_CMD_STARTED) {
            BT_ERR("%s, invalid status %d", __func__, status);
            break;
        }
#endif /* BT_DEV */
        break;
    }
    case BTA_GATTC_CLOSE_EVT:
        BT_DBG("BTA_GATTC_CLOSE_EVT");
        break;
    case BTA_GATTC_CONNECT_EVT: {
        BT_DBG("BTA_GATTC_CONNECT_EVT");

        if (bt_mesh_gattc_if != p_data->connect.client_if) {
            BT_ERR("bt_mesh_gattc_if & connect_if don't match");
            return;
        }

        if (bt_mesh_gattc_conn_cb != NULL && bt_mesh_gattc_conn_cb->connected != NULL) {
            for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
                if (!memcmp(bt_mesh_gattc_info[i].addr, p_data->connect.remote_bda, BD_ADDR_LEN)) {
                    bt_mesh_gattc_info[i].conn.handle = BT_MESH_GATT_GET_CONN_ID(p_data->connect.conn_id);
                    (bt_mesh_gattc_conn_cb->connected)(bt_mesh_gattc_info[i].addr, &bt_mesh_gattc_info[i].conn, i);
                    break;
                }
            }
        }
        break;
    }
    case BTA_GATTC_DISCONNECT_EVT: {
        BT_DBG("BTA_GATTC_DISCONNECT_EVT");

        if (bt_mesh_gattc_if != p_data->disconnect.client_if) {
            BT_ERR("bt_mesh_gattc_if & disconnect_if don't match");
            return;
        }

        handle = BT_MESH_GATT_GET_CONN_ID(p_data->disconnect.conn_id);

        if (bt_mesh_gattc_conn_cb != NULL && bt_mesh_gattc_conn_cb->disconnected != NULL) {
            for (i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
                if (!memcmp(bt_mesh_gattc_info[i].addr, p_data->disconnect.remote_bda, BD_ADDR_LEN)) {
                    if (bt_mesh_gattc_info[i].conn.handle == handle) {
                        (bt_mesh_gattc_conn_cb->disconnected)(&bt_mesh_gattc_info[i].conn, p_data->disconnect.reason);
                        if (!bt_mesh_gattc_info[i].wr_desc_done) {
                            /* Add this in case connection is established, connected event comes, but
                             * connection is terminated before server->filter_type is set to PROV.
                             */
                            provisioner_clear_link_conn_info(bt_mesh_gattc_info[i].addr);
                        }
                    } else {
                        /* Add this in case connection is failed to be established, and here we
                         * need to clear some provision link info, like connecting flag, device
                         * uuid, address info, etc.
                         */
                        provisioner_clear_link_conn_info(bt_mesh_gattc_info[i].addr);
                    }
                    /* Decrease prov pbg_count */
                    provisioner_pbg_count_dec();
                    /* Reset corresponding gattc info */
                    memset(&bt_mesh_gattc_info[i], 0, sizeof(bt_mesh_gattc_info[i]));
                    bt_mesh_gattc_info[i].conn.handle = 0xFFFF;
                    bt_mesh_gattc_info[i].mtu = GATT_DEF_BLE_MTU_SIZE;
                    bt_mesh_gattc_info[i].wr_desc_done = false;
                    break;
                }
            }
        }
        break;
    }
    case BTA_GATTC_CONGEST_EVT:
        break;
    case BTA_GATTC_SRVC_CHG_EVT:
        break;
    default:
        break;
    }
}
#endif /* defined(CONFIG_BT_MESH_PROVISIONER) && CONFIG_BT_MESH_PROVISIONER */

struct bt_conn *bt_mesh_conn_ref(struct bt_conn *conn)
{
    atomic_inc(&conn->ref);

    BT_DBG("handle %u ref %u", conn->handle, atomic_get(&conn->ref));

    return conn;
}

void bt_mesh_conn_unref(struct bt_conn *conn)
{
    atomic_dec(&conn->ref);

    BT_DBG("handle %u ref %u", conn->handle, atomic_get(&conn->ref));
}

void bt_mesh_gatt_init(void)
{
    tBT_UUID app_uuid = {LEN_UUID_128, {0}};

    BTA_GATT_SetLocalMTU(GATT_DEF_BLE_MTU_SIZE);

#if CONFIG_BT_MESH_NODE
    /* Fill our internal UUID with a fixed pattern 0x96 for the ble mesh */
    memset(&app_uuid.uu.uuid128, 0x96, LEN_UUID_128);
    BTA_GATTS_AppRegister(&app_uuid, bt_mesh_bta_gatts_cb);
#endif

#if CONFIG_BT_MESH_PROVISIONER
    for (int i = 0; i < ARRAY_SIZE(bt_mesh_gattc_info); i++) {
        bt_mesh_gattc_info[i].conn.handle = 0xFFFF;
        bt_mesh_gattc_info[i].mtu = GATT_DEF_BLE_MTU_SIZE; /* Default MTU_SIZE 23 */
        bt_mesh_gattc_info[i].wr_desc_done = false;
    }
    memset(&app_uuid.uu.uuid128, BT_MESH_GATTC_APP_UUID_BYTE, LEN_UUID_128);
    BTA_GATTC_AppRegister(&app_uuid, bt_mesh_bta_gattc_cb);
#endif
}

void bt_mesh_adapt_init(void)
{
    BT_DBG("%s", __func__);
    /* initialization of P-256 parameters */
    p_256_init_curve(KEY_LENGTH_DWORDS_P256);
}

int bt_mesh_rand(void *buf, size_t len)
{
    if (!len) {
        return -EAGAIN;
    }

    // Reset the buf value to the fixed value.
    memset(buf, 0x55, len);

    for (int i = 0; i < (int)(len / sizeof(u32_t)); i++) {
        u32_t rand = esp_random();
        memcpy(buf + i * sizeof(u32_t), &rand, sizeof(u32_t));
    }

    BT_DBG("%s, rand: %s", __func__, bt_hex(buf, len));
    return 0;
}

void bt_mesh_set_private_key(const u8_t pri_key[32])
{
    memcpy(bt_mesh_private_key, pri_key, 32);
}

const u8_t *bt_mesh_pub_key_get(void)
{
    Point public_key;
    BT_OCTET32 pri_key;
#if 1
    if (atomic_test_bit(bt_dev.flags, BT_DEV_HAS_PUB_KEY)) {
        return bt_mesh_public_key;
    }
#else
    /* BLE Mesh BQB test case MESH/NODE/PROV/UPD/BV-12-C requires
     * different public key for each provisioning procedure.
     * Note: if enabled, when Provisioner provision multiple devices
     * at the same time, this may cause invalid confirmation value.
     */
    if (bt_mesh_rand(bt_mesh_private_key, 32)) {
        BT_ERR("Unable to generate bt_mesh_private_key");
        return NULL;
    }
#endif
    mem_rcopy(pri_key, bt_mesh_private_key, 32);
    ECC_PointMult(&public_key, &(curve_p256.G), (DWORD *)pri_key, KEY_LENGTH_DWORDS_P256);

    memcpy(bt_mesh_public_key, public_key.x, BT_OCTET32_LEN);
    memcpy(bt_mesh_public_key + BT_OCTET32_LEN, public_key.y, BT_OCTET32_LEN);

    atomic_set_bit(bt_dev.flags, BT_DEV_HAS_PUB_KEY);
    BT_DBG("gen the bt_mesh_public_key:%s", bt_hex(bt_mesh_public_key, sizeof(bt_mesh_public_key)));

    return bt_mesh_public_key;
}

bool bt_mesh_check_public_key(const u8_t key[64])
{
    struct p256_pub_key {
        u8_t x[32];
        u8_t y[32];
    } check = {0};

    sys_memcpy_swap(check.x, key, 32);
    sys_memcpy_swap(check.y, key + 32, 32);

    return ECC_CheckPointIsInElliCur_P256((Point *)&check);
}

int bt_mesh_dh_key_gen(const u8_t remote_pk[64], bt_dh_key_cb_t cb)
{
    BT_OCTET32 private_key;
    Point peer_publ_key;
    Point new_publ_key;
    BT_OCTET32 dhkey;

    BT_DBG("private key = %s", bt_hex(bt_mesh_private_key, BT_OCTET32_LEN));

    mem_rcopy(private_key, bt_mesh_private_key, BT_OCTET32_LEN);
    memcpy(peer_publ_key.x, remote_pk, BT_OCTET32_LEN);
    memcpy(peer_publ_key.y, &remote_pk[BT_OCTET32_LEN], BT_OCTET32_LEN);

    BT_DBG("remote public key x = %s", bt_hex(peer_publ_key.x, BT_OCTET32_LEN));
    BT_DBG("remote public key y = %s", bt_hex(peer_publ_key.y, BT_OCTET32_LEN));

    ECC_PointMult(&new_publ_key, &peer_publ_key, (DWORD *) private_key, KEY_LENGTH_DWORDS_P256);

    memcpy(dhkey, new_publ_key.x, BT_OCTET32_LEN);

    BT_DBG("new public key x = %s", bt_hex(new_publ_key.x, 32));
    BT_DBG("new public key y = %s", bt_hex(new_publ_key.y, 32));

    if (cb != NULL) {
        cb((const u8_t *)dhkey);
    }

    return 0;
}

static void ecb_encrypt(u8_t const *const key_le, u8_t const *const clear_text_le,
                        u8_t *const cipher_text_le, u8_t *const cipher_text_be)
{
    struct ecb_param ecb;
    mbedtls_aes_context aes_ctx = {0};

    aes_ctx.key_bytes = 16;
    mem_rcopy(&aes_ctx.key[0], key_le, 16);
    mem_rcopy(&ecb.clear_text[0], clear_text_le, sizeof(ecb.clear_text));
    mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_ENCRYPT, &ecb.clear_text[0], &ecb.cipher_text[0]);

    if (cipher_text_le) {
        mem_rcopy(cipher_text_le, &ecb.cipher_text[0],
                  sizeof(ecb.cipher_text));
    }

    if (cipher_text_be) {
        memcpy(cipher_text_be, &ecb.cipher_text[0],
               sizeof(ecb.cipher_text));
    }
}

static void ecb_encrypt_be(u8_t const *const key_be, u8_t const *const clear_text_be,
                           u8_t *const cipher_text_be)
{
    struct ecb_param ecb;
    mbedtls_aes_context aes_ctx = {0};

    aes_ctx.key_bytes = 16;
    memcpy(&aes_ctx.key[0], key_be, 16);
    memcpy(&ecb.clear_text[0], clear_text_be, sizeof(ecb.clear_text));
    mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_ENCRYPT, &ecb.clear_text[0], &ecb.cipher_text[0]);

    memcpy(cipher_text_be, &ecb.cipher_text[0], sizeof(ecb.cipher_text));
}

int bt_encrypt_le(const u8_t key[16], const u8_t plaintext[16],
                  u8_t enc_data[16])
{
#if CONFIG_MBEDTLS_HARDWARE_AES
    BT_DBG("key %s plaintext %s", bt_hex(key, 16), bt_hex(plaintext, 16));

    ecb_encrypt(key, plaintext, enc_data, NULL);

    BT_DBG("enc_data %s", bt_hex(enc_data, 16));
    return 0;
#else /* CONFIG_MBEDTLS_HARDWARE_AES */
    struct tc_aes_key_sched_struct s;
    u8_t tmp[16];

    BT_DBG("key %s plaintext %s", bt_hex(key, 16), bt_hex(plaintext, 16));

    sys_memcpy_swap(tmp, key, 16);

    if (tc_aes128_set_encrypt_key(&s, tmp) == TC_CRYPTO_FAIL) {
        return -EINVAL;
    }

    sys_memcpy_swap(tmp, plaintext, 16);

    if (tc_aes_encrypt(enc_data, tmp, &s) == TC_CRYPTO_FAIL) {
        return -EINVAL;
    }

    sys_mem_swap(enc_data, 16);

    BT_DBG("enc_data %s", bt_hex(enc_data, 16));

    return 0;
#endif /* CONFIG_MBEDTLS_HARDWARE_AES */
}

int bt_encrypt_be(const u8_t key[16], const u8_t plaintext[16],
                  u8_t enc_data[16])
{
#if CONFIG_MBEDTLS_HARDWARE_AES
    BT_DBG("key %s plaintext %s", bt_hex(key, 16), bt_hex(plaintext, 16));

    ecb_encrypt_be(key, plaintext, enc_data);

    BT_DBG("enc_data %s", bt_hex(enc_data, 16));

    return 0;
#else /* CONFIG_MBEDTLS_HARDWARE_AES */
    struct tc_aes_key_sched_struct s;

    BT_DBG("key %s plaintext %s", bt_hex(key, 16), bt_hex(plaintext, 16));

    if (tc_aes128_set_encrypt_key(&s, key) == TC_CRYPTO_FAIL) {
        return -EINVAL;
    }

    if (tc_aes_encrypt(enc_data, plaintext, &s) == TC_CRYPTO_FAIL) {
        return -EINVAL;
    }

    BT_DBG("enc_data %s", bt_hex(enc_data, 16));

    return 0;
#endif /* CONFIG_MBEDTLS_HARDWARE_AES */
}

#if defined(CONFIG_BT_MESH_USE_DUPLICATE_SCAN)
int bt_mesh_update_exceptional_list(u8_t sub_code, u8_t type, void *info)
{
    BD_ADDR value = {0};

    if ((sub_code > BT_MESH_EXCEP_LIST_CLEAN) ||
        (type > BT_MESH_EXCEP_INFO_MESH_PROXY_ADV)) {
        BT_ERR("%s, Invalid parameter", __func__);
        return -EINVAL;
    }

    if (type == BT_MESH_EXCEP_INFO_MESH_LINK_ID) {
        if (!info) {
            BT_ERR("%s, NULL Provisioning Link ID", __func__);
            return -EINVAL;
        }
        memcpy(value, info, sizeof(u32_t));
    }

    BT_DBG("%s, %s type 0x%x", __func__, sub_code ? "Remove" : "Add", type);

    /* The parameter "device_info" can't be NULL in the API */
    BLE_MESH_BTM_CHECK_STATUS(BTM_UpdateBleDuplicateExceptionalList(sub_code, type, value, NULL));

    return 0;
}
#endif

#endif /* #if CONFIG_BT_MESH */

