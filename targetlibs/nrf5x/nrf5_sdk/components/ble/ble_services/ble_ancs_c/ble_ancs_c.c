/* Copyright (c) 2012 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 */
 
/* Disclaimer: This client implementation of the Apple Notification Center Service can and will be changed at any time by Nordic Semiconductor ASA.
 * Server implementations such as the ones found in iOS can be changed at any time by Apple and may cause this client implementation to stop working.
 */

#include "ble_ancs_c.h"
#include "ble_err.h"
#include "ble_srv_common.h"
#include "nrf_assert.h"
#include "device_manager.h"
#include "ble_db_discovery.h"
#include "app_error.h"
#include "app_trace.h"
#include "sdk_common.h"

#define BLE_ANCS_NOTIF_EVT_ID_INDEX       0                       /**< Index of the Event ID field when parsing notifications. */
#define BLE_ANCS_NOTIF_FLAGS_INDEX        1                       /**< Index of the Flags field when parsing notifications. */
#define BLE_ANCS_NOTIF_CATEGORY_ID_INDEX  2                       /**< Index of the Category ID field when parsing notifications. */
#define BLE_ANCS_NOTIF_CATEGORY_CNT_INDEX 3                       /**< Index of the Category Count field when parsing notifications. */
#define BLE_ANCS_NOTIF_NOTIF_UID          4                       /**< Index of the Notification UID field when patsin notifications. */

#define LOG                              app_trace_log            /**< Debug logger macro that will be used in this file to do logging of important information over UART. */
#define LOG_DUMP                         app_trace_dump           /**< Debug logger macro that will be used in this file to do logging of important information over UART. */

#define START_HANDLE_DISCOVER            0x0001                   /**< Value of start handle during discovery. */

#define TX_BUFFER_MASK                   0x07                     /**< TX buffer mask. Must be a mask of contiguous zeroes followed by a contiguous sequence of ones: 000...111. */
#define TX_BUFFER_SIZE                   (TX_BUFFER_MASK + 1)     /**< Size of send buffer, which is 1 higher than the mask. */
#define WRITE_MESSAGE_LENGTH             20                       /**< Length of the write message for CCCD/control point. */
#define BLE_CCCD_NOTIFY_BIT_MASK         0x0001                   /**< Enable notification bit. */

#define BLE_ANCS_MAX_DISCOVERED_CENTRALS DEVICE_MANAGER_MAX_BONDS /**< Maximum number of discovered services that can be stored in the flash. This number should be identical to maximum number of bonded peer devices. */

#define TIME_STRING_LEN                  15                       /**< Unicode Technical Standard (UTS) #35 date format pattern "yyyyMMdd'T'HHmmSS" + "'\0'". */

#define DISCOVERED_SERVICE_DB_SIZE \
    CEIL_DIV(sizeof(ble_ancs_c_service_t) * BLE_ANCS_MAX_DISCOVERED_CENTRALS, sizeof(uint32_t)) /**< Size of bonded peer's database in word size (4 byte). */


/**@brief ANCS request types.
 */
typedef enum
{
    READ_REQ = 1,  /**< Type identifying that this tx_message is a read request. */
    WRITE_REQ      /**< Type identifying that this tx_message is a write request. */
} ancs_tx_request_t;


/**@brief Structure used for holding the characteristic found during the discovery process.
 */
typedef struct
{
    ble_uuid_t            uuid;          /**< UUID identifying the characteristic. */
    ble_gatt_char_props_t properties;    /**< Properties for the characteristic. */
    uint16_t              handle_decl;   /**< Characteristic Declaration Handle for the characteristic. */
    uint16_t              handle_value;  /**< Value Handle for the value provided in the characteristic. */
    uint16_t              handle_cccd;   /**< CCCD Handle value for the characteristic. */
} ble_ancs_c_characteristic_t;


/**@brief Structure used for holding the Apple Notification Center Service found during the discovery process.
 */
typedef struct
{
    uint8_t                     handle;         /**< Handle of Apple Notification Center Service, which identifies to which peer this discovered service belongs. */
    ble_gattc_service_t         service;        /**< The GATT Service holding the discovered Apple Notification Center Service. */
    ble_ancs_c_characteristic_t control_point;  /**< Control Point Characteristic for the service. Allows interaction with the peer. */
    ble_ancs_c_characteristic_t notif_source;   /**< Characteristic that keeps track of arrival, modification, and removal of notifications. */
    ble_ancs_c_characteristic_t data_source;    /**< Characteristic where attribute data for the notifications is received from peer. */
} ble_ancs_c_service_t;


/**@brief Structure for writing a message to the central, i.e. Control Point or CCCD.
 */
typedef struct
{
    uint8_t                  gattc_value[WRITE_MESSAGE_LENGTH]; /**< The message to write. */
    ble_gattc_write_params_t gattc_params;                      /**< GATTC parameters for this message. */
} write_params_t;


/**@brief Structure for holding data to be transmitted to the connected master.
 */
typedef struct
{
    uint16_t          conn_handle;  /**< Connection handle to be used when transmitting this message. */
    ancs_tx_request_t type;         /**< Type of this message, i.e. read or write message. */
    union
    {
        uint16_t       read_handle; /**< Read request message. */
        write_params_t write_req;   /**< Write request message. */
    } req;
} tx_message_t;


/**@brief Parsing states for received iOS notification attributes.
 */
typedef enum
{
    COMMAND_ID_AND_NOTIF_UID,  /**< Parsing the command ID and the notification UID. */
    ATTR_ID,                   /**< Parsing attribute ID. */
    ATTR_LEN1,                 /**< Parsing the LSB of the attribute length. */
    ATTR_LEN2,                 /**< Parsing the MSB of the attribute length. */
    ATTR_DATA,                 /**< Parsing the attribute data. */
    DONE                       /**< Parsing is done. */
} ble_ancs_c_parse_state_t;

static tx_message_t m_tx_buffer[TX_BUFFER_SIZE];                           /**< Transmit buffer for messages to be transmitted to the Notification Provider. */
static uint32_t     m_tx_insert_index = 0;                                 /**< Current index in the transmit buffer where the next message should be inserted. */
static uint32_t     m_tx_index        = 0;                                 /**< Current index in the transmit buffer from where the next message to be transmitted resides. */

static ble_ancs_c_service_t   m_service;                                   /**< Current service data. */
static ble_ancs_c_t         * mp_ble_ancs;                                 /**< Pointer to the current instance of the ANCS client module. The memory for this is provided by the application.*/
static ble_ancs_c_attr_list_t m_ancs_attr_list[BLE_ANCS_NB_OF_ATTRS];      /**< For all attributes; contains whether they should be requested upon attribute request and the length and buffer of where to store attribute data. */
static uint32_t               m_expected_number_of_attrs;                  /**< Variable to keep track of when to stop reading incoming attributes. */
static ble_ancs_c_evt_t       m_ancs_evt;                                  /**< The ANCS event that is created in this module and propagated to the application. */

static ble_ancs_c_parse_state_t m_parse_state = COMMAND_ID_AND_NOTIF_UID;  /**< ANCS notification attribute parsing state. */


/**@brief 128-bit service UUID for the Apple Notification Center Service.
 */
const ble_uuid128_t ble_ancs_base_uuid128 =
{
    {
        // 7905F431-B5CE-4E99-A40F-4B1E122D00D0
        0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4,
        0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79
    }
};


/**@brief 128-bit control point UUID.
 */
const ble_uuid128_t ble_ancs_cp_base_uuid128 =
{
    {
        // 69d1d8f3-45e1-49a8-9821-9BBDFDAAD9D9
        0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98,
        0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69
    }
};

/**@brief 128-bit notification source UUID.
*/
const ble_uuid128_t ble_ancs_ns_base_uuid128 =
{
    {
        // 9FBF120D-6301-42D9-8C58-25E699A21DBD
        0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c,
        0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f

    }
};

/**@brief 128-bit data source UUID.
*/
const ble_uuid128_t ble_ancs_ds_base_uuid128 =
{
    {
        // 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
        0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe,
        0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22
    }
};


/**@brief Function for handling events from the database discovery module.
 *
 * @details This function handles events from the database discovery module and determines
 *          if it relates to the discovery of the Apple Notification Center Service at the 
 *          peer. If so, it will call the application's event handler indicating that the Apple
 *          Notification Center Service has been discovered at the peer. It also populates the
 *          event with the service-related information before providing it to the application.
 *
 * @param[in] p_evt Pointer to the event received from the database discovery module.
 */
static void db_discover_evt_handler(ble_db_discovery_evt_t * p_evt)
{
    LOG("[ANCS]: Database Discovery handler called with event 0x%x\r\n", p_evt->evt_type);

    ble_ancs_c_evt_t evt;
    ble_gatt_db_char_t * p_chars;

    p_chars = p_evt->params.discovered_db.charateristics;

    // Check if the ANCS Service was discovered.
    if (p_evt->evt_type == BLE_DB_DISCOVERY_COMPLETE &&
        p_evt->params.discovered_db.srv_uuid.uuid == ANCS_UUID_SERVICE &&
        p_evt->params.discovered_db.srv_uuid.type == BLE_UUID_TYPE_VENDOR_BEGIN)
    {
        mp_ble_ancs->conn_handle = p_evt->conn_handle;

        // Find the handles of the ANCS characteristic.
        uint32_t i;

        for (i = 0; i < p_evt->params.discovered_db.char_count; i++)
        {
            switch (p_evt->params.discovered_db.charateristics[i].characteristic.uuid.uuid)
            {
                case ANCS_UUID_CHAR_CONTROL_POINT:
                    LOG("[ANCS]: Control Point Characteristic found.\n\r");
                    m_service.control_point.properties   = p_chars[i].characteristic.char_props;
                    m_service.control_point.handle_decl  = p_chars[i].characteristic.handle_decl;
                    m_service.control_point.handle_value = p_chars[i].characteristic.handle_value;
                    m_service.control_point.handle_cccd  = p_chars[i].cccd_handle;
                    break;

                case ANCS_UUID_CHAR_DATA_SOURCE:
                    LOG("[ANCS]: Data Source Characteristic found.\n\r");
                    m_service.data_source.properties   = p_chars[i].characteristic.char_props;
                    m_service.data_source.handle_decl  = p_chars[i].characteristic.handle_decl;
                    m_service.data_source.handle_value = p_chars[i].characteristic.handle_value;
                    m_service.data_source.handle_cccd  = p_chars[i].cccd_handle;
                    break;

                case ANCS_UUID_CHAR_NOTIFICATION_SOURCE:
                    LOG("[ANCS]: Notification point Characteristic found.\n\r");
                    m_service.notif_source.properties   = p_chars[i].characteristic.char_props;
                    m_service.notif_source.handle_decl  = p_chars[i].characteristic.handle_decl;
                    m_service.notif_source.handle_value = p_chars[i].characteristic.handle_value;
                    m_service.notif_source.handle_cccd  = p_chars[i].cccd_handle;
                    break;

                default:
                    break;
            }
        }
        evt.evt_type = BLE_ANCS_C_EVT_DISCOVER_COMPLETE;
        mp_ble_ancs->evt_handler(&evt);
    }
    else
    {
        evt.evt_type = BLE_ANCS_C_EVT_DISCOVER_FAILED;
        mp_ble_ancs->evt_handler(&evt);
    }
}


/**@brief Function for passing any pending request from the buffer to the stack.
 */
static void tx_buffer_process(void)
{
    if (m_tx_index != m_tx_insert_index)
    {
        uint32_t err_code;

        if (m_tx_buffer[m_tx_index].type == READ_REQ)
        {
            err_code = sd_ble_gattc_read(m_tx_buffer[m_tx_index].conn_handle,
                                         m_tx_buffer[m_tx_index].req.read_handle,
                                         0);
        }
        else
        {
            err_code = sd_ble_gattc_write(m_tx_buffer[m_tx_index].conn_handle,
                                          &m_tx_buffer[m_tx_index].req.write_req.gattc_params);
        }
        if (err_code == NRF_SUCCESS)
        {
            ++m_tx_index;
            m_tx_index &= TX_BUFFER_MASK;
        }
    }
}


/**@brief Function for parsing received notification attribute response data.
 *
 * @details The data that comes from the Notification Provider can be much longer than what
 *          would fit in a single GATTC notification. Therefore, function relies on static
 *          variables and a state-oriented switch case.
 *          UID and command ID will be received only once at the beginning of the first 
 *          GATTC notification of a new attribute request for a given iOS notification.
 *          After this, we can loop several ID > LENGTH > DATA > ID > LENGTH > DATA until we have
 *          received all attributes we wanted as a Notification Consumer. The Notification Provider
 *          can also simply stop sending attributes.
 *
 * @param[in] p_ancs     Pointer to an ANCS instance to which the event belongs.
 * @param[in] p_data_src Pointer to data that was received from the Notification Provider.
 * @param[in] hvx_len    Length of the data that was received from the Notification Provider.
 */
static void parse_get_notif_attrs_response(ble_ancs_c_t  * p_ancs,
                                           const uint8_t * p_data_src,
                                           const uint16_t  hvx_data_len)
{
    static uint8_t               * p_data_dest;
    static uint16_t                current_attr_index;
    static ble_ancs_c_evt_t        evt;
    ble_ancs_c_command_id_values_t command_id;
    uint32_t                       index;

    evt.ancs_attr_list = m_ancs_attr_list;

    for (index = 0; index < hvx_data_len;)
    {
        switch (m_parse_state)
        {
            case COMMAND_ID_AND_NOTIF_UID:
                command_id = (ble_ancs_c_command_id_values_t) p_data_src[index++];
                if(command_id != BLE_ANCS_COMMAND_ID_GET_NOTIF_ATTRIBUTES)
                {
                    LOG("[ANCS]: Invalid Command ID");
                    m_parse_state = DONE;
                }

                evt.attr.notif_uid  = uint32_decode(&p_data_src[index]);
                index              += sizeof(uint32_t);
                m_parse_state       = ATTR_ID;

                if (evt.attr.notif_uid != m_ancs_evt.notif.notif_uid)
                {
                    LOG("UID mismatch: Notification UID %x , Attribute UID %x\n\r",
                        evt.notif.notif_uid,
                        evt.attr.notif_uid);
                    m_parse_state = DONE;
                }
                break;

            case ATTR_ID:
                if (m_expected_number_of_attrs == 0)
                {
                    LOG("[ANCS]: All requested attributes received\n\r");
                    m_parse_state = DONE;
                    index++;
                }
                else
                {
                    evt.attr.attr_id = (ble_ancs_c_notif_attr_id_values_t) p_data_src[index++];
                    p_data_dest        = m_ancs_attr_list[evt.attr.attr_id].p_attr_data;
                    if (m_ancs_attr_list[evt.attr.attr_id].get == true)
                    {
                        m_parse_state = ATTR_LEN1;
                    }
                    m_expected_number_of_attrs--;
                    LOG("Attribute ID %i \n\r", evt.attr.attr_id);
                }
                break;

            case ATTR_LEN1:
                evt.attr.attr_len = p_data_src[index++];
                m_parse_state       = ATTR_LEN2;
                break;

            case ATTR_LEN2:
                evt.attr.attr_len |= (p_data_src[index++] << 8);
                current_attr_index   = 0;
                if (evt.attr.attr_len != 0)
                {
                    m_parse_state = ATTR_DATA;
                }
                else
                {
                    evt.evt_type = BLE_ANCS_C_EVT_NOTIF_ATTRIBUTE;
                    p_ancs->evt_handler(&evt);
                    m_parse_state = ATTR_ID;
                }
                LOG("Attribute LEN %i \n\r", evt.attr.attr_len);
                break;

            case ATTR_DATA:
                // We have not reached the end of the attribute, nor our max allocated internal size.
                // Proceed with copying data over to our buffer.
                if (   (current_attr_index < m_ancs_attr_list[evt.attr.attr_id].attr_len)
                    && (current_attr_index < evt.attr.attr_len))
                {
                    p_data_dest[current_attr_index++] = p_data_src[index++];
                }
                // We have reached the end of the attribute, or our max allocated internal size.
                // Stop copying data over to our buffer. NUL-terminate at the current index.
                if ( (current_attr_index == evt.attr.attr_len) ||
                     (current_attr_index == m_ancs_attr_list[evt.attr.attr_id].attr_len))
                {
                    p_data_dest[current_attr_index] = '\0';
                    
                    // If our max buffer size is smaller than the remaining attribute data, we must
                    // increase index to skip the data until the start of the next attribute.
                    if (current_attr_index < evt.attr.attr_len)
                    {
                        index += (evt.attr.attr_len - current_attr_index);
                    }
                    m_parse_state = ATTR_ID;
                    LOG("Attribute finished!\n\r");
                    evt.evt_type = BLE_ANCS_C_EVT_NOTIF_ATTRIBUTE;
                    p_ancs->evt_handler(&evt);
                }
                break;

            case DONE:
                index = hvx_data_len;
                break;

            default:
                // Default case will never trigger intentionally. Go to the DONE state to minimize the consequences.
                m_parse_state = DONE;
                break;
        }
    }
}


/**@brief Function for checking if data in an iOS notification is out of bounds.
 *
 * @param[in] notif  An iOS notification.
 *
 * @retval NRF_SUCCESS             If the notification is within bounds.
 * @retval NRF_ERROR_INVALID_PARAM If the notification is out of bounds.
 */
static uint32_t ble_ancs_verify_notification_format(const ble_ancs_c_evt_notif_t * notif)
{
    if(   (notif->evt_id >= BLE_ANCS_NB_OF_EVT_ID)
       || (notif->category_id >= BLE_ANCS_NB_OF_CATEGORY_ID))
    {
        return NRF_ERROR_INVALID_PARAM;
    }
    return NRF_SUCCESS;
}

/**@brief Function for receiving and validating notifications received from the Notification Provider.
 * 
 * @param[in] p_ancs     Pointer to an ANCS instance to which the event belongs.
 * @param[in] p_ble_evt  Bluetooth stack event.
 * @param[in] p_data_src Pointer to data that was received from the Notification Provider.
 * @param[in] hvx_len    Length of the data that was received by the Notification Provider.
 */
static void parse_notif(const ble_ancs_c_t * p_ancs,
                        ble_ancs_c_evt_t   * p_ancs_evt,
                        const uint8_t      * p_data_src,
                        const uint16_t       hvx_data_len)
{
    uint32_t err_code;
    if (hvx_data_len != BLE_ANCS_NOTIFICATION_DATA_LENGTH)
    {
        m_ancs_evt.evt_type = BLE_ANCS_C_EVT_INVALID_NOTIF;
        p_ancs->evt_handler(&m_ancs_evt);
    }

    /*lint --e{415} --e{416} -save suppress Warning 415: possible access out of bond */
    p_ancs_evt->notif.evt_id                    =
            (ble_ancs_c_evt_id_values_t) p_data_src[BLE_ANCS_NOTIF_EVT_ID_INDEX];

    p_ancs_evt->notif.evt_flags.silent          =
            (p_data_src[BLE_ANCS_NOTIF_FLAGS_INDEX] >> BLE_ANCS_EVENT_FLAG_SILENT) & 0x01;

    p_ancs_evt->notif.evt_flags.important       =
            (p_data_src[BLE_ANCS_NOTIF_FLAGS_INDEX] >> BLE_ANCS_EVENT_FLAG_IMPORTANT) & 0x01;

    p_ancs_evt->notif.evt_flags.pre_existing    =
            (p_data_src[BLE_ANCS_NOTIF_FLAGS_INDEX] >> BLE_ANCS_EVENT_FLAG_PREEXISTING) & 0x01;

    p_ancs_evt->notif.evt_flags.positive_action =
            (p_data_src[BLE_ANCS_NOTIF_FLAGS_INDEX] >> BLE_ANCS_EVENT_FLAG_POSITIVE_ACTION) & 0x01;

    p_ancs_evt->notif.evt_flags.negative_action =
            (p_data_src[BLE_ANCS_NOTIF_FLAGS_INDEX] >> BLE_ANCS_EVENT_FLAG_NEGATIVE_ACTION) & 0x01;

    p_ancs_evt->notif.category_id               =
        (ble_ancs_c_category_id_values_t) p_data_src[BLE_ANCS_NOTIF_CATEGORY_ID_INDEX];

    p_ancs_evt->notif.category_count            = p_data_src[BLE_ANCS_NOTIF_CATEGORY_CNT_INDEX];
    p_ancs_evt->notif.notif_uid = uint32_decode(&p_data_src[BLE_ANCS_NOTIF_NOTIF_UID]);
    /*lint -restore*/

    err_code = ble_ancs_verify_notification_format(&m_ancs_evt.notif);
    if (err_code == NRF_SUCCESS)
    {
        m_ancs_evt.evt_type = BLE_ANCS_C_EVT_NOTIF;
    }
    else
    {
        m_ancs_evt.evt_type = BLE_ANCS_C_EVT_INVALID_NOTIF;
    }

    p_ancs->evt_handler(&m_ancs_evt);
}


/**@brief Function for receiving and validating notifications received from the Notification Provider.
 * 
 * @param[in] p_ancs    Pointer to an ANCS instance to which the event belongs.
 * @param[in] p_ble_evt Bluetooth stack event.
 */
static void on_evt_gattc_notif(ble_ancs_c_t * p_ancs, const ble_evt_t * p_ble_evt)
{
    const ble_gattc_evt_hvx_t * p_notif = &p_ble_evt->evt.gattc_evt.params.hvx;

    if (p_notif->handle == m_service.notif_source.handle_value)
    {
        BLE_UUID_COPY_INST(m_ancs_evt.uuid, m_service.notif_source.uuid);
        parse_notif(p_ancs, &m_ancs_evt,p_notif->data,p_notif->len);
    }
    else if (p_notif->handle == m_service.data_source.handle_value)
    {
        BLE_UUID_COPY_INST(m_ancs_evt.uuid, m_service.data_source.uuid);
        parse_get_notif_attrs_response(p_ancs, p_notif->data, p_notif->len);
    }
    else
    {
        // No applicable action.
    }
}

/**@brief Function for handling write response events.
 */
static void on_evt_write_rsp()
{
    tx_buffer_process();
}


void ble_ancs_c_on_device_manager_evt(ble_ancs_c_t      * p_ans,
                                      dm_handle_t const * p_handle,
                                      dm_event_t const  * p_dm_evt)
{
    switch (p_dm_evt->event_id)
    {
        case DM_EVT_CONNECTION:
            // Fall through.
        case DM_EVT_SECURITY_SETUP_COMPLETE:
            p_ans->central_handle = p_handle->device_id;
            break;

        default:
            // Do nothing.
            break;
    }
}


void ble_ancs_c_on_ble_evt(ble_ancs_c_t * p_ancs, const ble_evt_t * p_ble_evt)
{
    uint16_t evt = p_ble_evt->header.evt_id;

    switch (evt)
    {
        case BLE_GATTC_EVT_WRITE_RSP:
            on_evt_write_rsp();
            break;

        case BLE_GATTC_EVT_HVX:
            on_evt_gattc_notif(p_ancs, p_ble_evt);
            break;

        default:
            break;
    }
}


uint32_t ble_ancs_c_init(ble_ancs_c_t * p_ancs, const ble_ancs_c_init_t * p_ancs_init)
{
    VERIFY_PARAM_NOT_NULL(p_ancs);
    VERIFY_PARAM_NOT_NULL(p_ancs_init);
    VERIFY_PARAM_NOT_NULL(p_ancs_init->evt_handler);

    mp_ble_ancs = p_ancs;

    mp_ble_ancs->evt_handler    = p_ancs_init->evt_handler;
    mp_ble_ancs->error_handler  = p_ancs_init->error_handler;
    mp_ble_ancs->service_handle = BLE_GATT_HANDLE_INVALID;
    mp_ble_ancs->central_handle = DM_INVALID_ID;
    mp_ble_ancs->conn_handle    = BLE_CONN_HANDLE_INVALID;

    memset(&m_service, 0, sizeof(ble_ancs_c_service_t));
    memset(m_tx_buffer, 0, TX_BUFFER_SIZE);

    m_service.handle = BLE_GATT_HANDLE_INVALID;

    ble_uuid_t ancs_uuid;
    ancs_uuid.uuid = ANCS_UUID_SERVICE;
    ancs_uuid.type = BLE_UUID_TYPE_VENDOR_BEGIN;

    return ble_db_discovery_evt_register(&ancs_uuid, db_discover_evt_handler);
}


/**@brief Function for creating a TX message for writing a CCCD.
 *
 * @param[in] conn_handle  Connection handle on which to perform the configuration.
 * @param[in] handle_cccd  Handle of the CCCD.
 * @param[in] enable       Enable or disable GATTC notifications.
 *
 * @retval NRF_SUCCESS              If the message was created successfully.
 * @retval NRF_ERROR_INVALID_PARAM  If one of the input parameters was invalid.
 */
static uint32_t cccd_configure(const uint16_t conn_handle, const uint16_t handle_cccd, bool enable)
{
    tx_message_t * p_msg;
    uint16_t       cccd_val = enable ? BLE_CCCD_NOTIFY_BIT_MASK : 0;

    p_msg              = &m_tx_buffer[m_tx_insert_index++];
    m_tx_insert_index &= TX_BUFFER_MASK;

    p_msg->req.write_req.gattc_params.handle   = handle_cccd;
    p_msg->req.write_req.gattc_params.len      = 2;
    p_msg->req.write_req.gattc_params.p_value  = p_msg->req.write_req.gattc_value;
    p_msg->req.write_req.gattc_params.offset   = 0;
    p_msg->req.write_req.gattc_params.write_op = BLE_GATT_OP_WRITE_REQ;
    p_msg->req.write_req.gattc_value[0]        = LSB_16(cccd_val);
    p_msg->req.write_req.gattc_value[1]        = MSB_16(cccd_val);
    p_msg->conn_handle                         = conn_handle;
    p_msg->type                                = WRITE_REQ;

    tx_buffer_process();
    return NRF_SUCCESS;
}


uint32_t ble_ancs_c_notif_source_notif_enable(const ble_ancs_c_t * p_ancs)
{
    LOG("[ANCS]: Enable Notification Source notifications. writing to handle: %i \n\r",
        m_service.notif_source.handle_cccd);
    return cccd_configure(p_ancs->conn_handle, m_service.notif_source.handle_cccd, true);
}


uint32_t ble_ancs_c_notif_source_notif_disable(const ble_ancs_c_t * p_ancs)
{
    return cccd_configure(p_ancs->conn_handle, m_service.notif_source.handle_cccd, false);
}


uint32_t ble_ancs_c_data_source_notif_enable(const ble_ancs_c_t * p_ancs)
{
    LOG("[ANCS]: Enable Data Source notifications. Writing to handle: %i \n\r",
        m_service.data_source.handle_cccd);
    return cccd_configure(p_ancs->conn_handle, m_service.data_source.handle_cccd, true);
}


uint32_t ble_ancs_c_data_source_notif_disable(const ble_ancs_c_t * p_ancs)
{
    return cccd_configure(p_ancs->conn_handle, m_service.data_source.handle_cccd, false);
}


uint32_t ble_ancs_get_notif_attrs(const ble_ancs_c_t * p_ancs,
                                  const uint32_t       p_uid)
{
    tx_message_t * p_msg;
    uint32_t       index                    = 0;
    uint32_t       number_of_requested_attr = 0;

    p_msg              = &m_tx_buffer[m_tx_insert_index++];
    m_tx_insert_index &= TX_BUFFER_MASK;

    p_msg->req.write_req.gattc_params.handle   = m_service.control_point.handle_value;
    p_msg->req.write_req.gattc_params.p_value  = p_msg->req.write_req.gattc_value;
    p_msg->req.write_req.gattc_params.offset   = 0;
    p_msg->req.write_req.gattc_params.write_op = BLE_GATT_OP_WRITE_REQ;

    //Encode Command ID.
    p_msg->req.write_req.gattc_value[index++] = BLE_ANCS_COMMAND_ID_GET_NOTIF_ATTRIBUTES;
    
    //Encode Notification UID.
    index += uint32_encode(p_uid, &p_msg->req.write_req.gattc_value[index]);

    //Encode Attribute ID.
    for (uint32_t attr = 0; attr < BLE_ANCS_NB_OF_ATTRS; attr++)
    {
        if (m_ancs_attr_list[attr].get == true)
        {
            p_msg->req.write_req.gattc_value[index++] = attr;
            if ((attr == BLE_ANCS_NOTIF_ATTR_ID_TITLE) ||
                (attr == BLE_ANCS_NOTIF_ATTR_ID_SUBTITLE) ||
                (attr == BLE_ANCS_NOTIF_ATTR_ID_MESSAGE))
            {
                //Encode Length field, only applicable for Title, Subtitle and Message
                index += uint16_encode(m_ancs_attr_list[attr].attr_len,
                              &p_msg->req.write_req.gattc_value[index]);
            }
            number_of_requested_attr++;
        }
    }
    p_msg->req.write_req.gattc_params.len = index;
    p_msg->conn_handle                    = p_ancs->conn_handle;
    p_msg->type                           = WRITE_REQ;
    m_expected_number_of_attrs            = number_of_requested_attr;

    tx_buffer_process();

    return NRF_SUCCESS;
}


uint32_t ble_ancs_c_attr_add(const ble_ancs_c_notif_attr_id_values_t id,
                             uint8_t                               * p_data,
                             const uint16_t                          len)
{
    VERIFY_PARAM_NOT_NULL(p_data);

    if((len == 0) || (len > BLE_ANCS_ATTR_DATA_MAX))
    {
        return NRF_ERROR_INVALID_LENGTH;
    }

    m_ancs_attr_list[id].get         = true;
    m_ancs_attr_list[id].attr_len    = len;
    m_ancs_attr_list[id].p_attr_data = p_data;

    return NRF_SUCCESS;
}


uint32_t ble_ancs_c_request_attrs(const ble_ancs_c_evt_notif_t * notif)
{
    uint32_t err_code;
    err_code = ble_ancs_verify_notification_format(notif);
    VERIFY_SUCCESS(err_code);

    err_code      = ble_ancs_get_notif_attrs(mp_ble_ancs, notif->notif_uid);
    m_parse_state = COMMAND_ID_AND_NOTIF_UID;
    VERIFY_SUCCESS(err_code);

    return NRF_SUCCESS;
}

