#include "nvs_flash.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "string.h"
#include "driver/gpio.h"
#include "esp_zb_main.h"

static const char *TAG = "DEMO";

// zcl strings start with their size
static void esp_zb_set_zcl_string(char *buffer, char *value)
{
    buffer[0] = (uint8_t)strlen(value);
    memcpy(buffer + 1, value, buffer[0]);
}

// report attribute to the coordinator
static void reportAttribute(uint8_t endpoint, uint16_t clusterID, uint16_t attributeID, void *value)
{
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd.src_endpoint = endpoint,
        .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
        .clusterID = clusterID,
        .attributeID = attributeID,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
    };
    esp_zb_zcl_set_attribute_val(endpoint, clusterID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attributeID, value, false);
    esp_zb_zcl_report_attr_cmd_req(&cmd);
}

// button task: report the button state to the coordinator when it changes
void button_task(void *pvParameters)
{
    uint8_t last_state = 0;
    while (1)
    {
        uint8_t button_state = gpio_get_level(GPIO_NUM_12);
        if (button_state != last_state)
        {
            ESP_LOGI(TAG, "Button changed: %d", button_state);
            last_state = button_state;
            reportAttribute(HA_BUTTON_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT, ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID, &button_state);
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// zigbee attribute handler
// dispacth the zigbee messages to the right handler
// according to their endpoint/cluster/attribute ids/types/values
static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    bool light_state = 0;
    uint8_t light_level = 0;
    uint16_t light_color_x = 0;
    uint16_t light_color_y = 0;
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
                        message->info.status);
    ESP_LOGI(TAG, "Received message: endpoint(0x%x), cluster(0x%x), attribute(0x%x), type(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.type, message->attribute.data.size);

    // here we only handle the light endpoint
    if (message->info.dst_endpoint == HA_LIGHT_ENDPOINT)
    {
        switch (message->info.cluster)
        {
        // handle on/off messages for the light
        case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL)
            {
                light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
                ESP_LOGI(TAG, "Light sets to %s", light_state ? "On" : "Off");
                light_driver_set_power(light_state);
            }
            else
            {
                ESP_LOGW(TAG, "On/Off cluster data: attribute(0x%x), type(0x%x)", message->attribute.id, message->attribute.data.type);
            }
            break;
        // handle color messages for the light
        case ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16)
            {
                light_color_x = message->attribute.data.value ? *(uint16_t *)message->attribute.data.value : light_color_x;
                light_color_y = *(uint16_t *)esp_zb_zcl_get_attribute(message->info.dst_endpoint, message->info.cluster,
                                                                      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID)
                                     ->data_p;
                ESP_LOGI(TAG, "Light color x changes to 0x%x", light_color_x);
            }
            else if (message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID &&
                     message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16)
            {
                light_color_y = message->attribute.data.value ? *(uint16_t *)message->attribute.data.value : light_color_y;
                light_color_x = *(uint16_t *)esp_zb_zcl_get_attribute(message->info.dst_endpoint, message->info.cluster,
                                                                      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID)
                                     ->data_p;
                ESP_LOGI(TAG, "Light color y changes to 0x%x", light_color_y);
            }
            else
            {
                ESP_LOGW(TAG, "Color control cluster data: attribute(0x%x), type(0x%x)", message->attribute.id, message->attribute.data.type);
            }
            light_driver_set_color_xy(light_color_x, light_color_y);
            break;
        // handle level messages for the light
        case ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL:
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8)
            {
                light_level = message->attribute.data.value ? *(uint8_t *)message->attribute.data.value : light_level;
                light_driver_set_level((uint8_t)light_level);
                ESP_LOGI(TAG, "Light level changes to %d", light_level);
            }
            else
            {
                ESP_LOGW(TAG, "Level Control cluster data: attribute(0x%x), type(0x%x)", message->attribute.id, message->attribute.data.type);
            }
            break;
        // handle the rest
        default:
            ESP_LOGI(TAG, "Message data: cluster(0x%x), attribute(0x%x)", message->info.cluster, message->attribute.id);
        }
    }
    return ret;
}

// zigbee action handler
// handles the zigbee stack actions and if the device receives a message
// it calls the attribute handler
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id)
    {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

esp_zb_cluster_list_t *esp_zb_create_button_cluster_list()
{
    // 1- clusters creation
    // see: https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32/api-reference/esp_zigbee_cluster.html

    // basic cluster
    esp_zb_basic_cluster_cfg_t basic_cluster_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x03, // mains
    };
    char manufacturer_name[16];
    esp_zb_set_zcl_string(manufacturer_name, "xseignard");
    char model_identifier[16];
    esp_zb_set_zcl_string(model_identifier, "demo");
    esp_zb_attribute_list_t *esp_zb_basic_cluster = esp_zb_basic_cluster_create(&basic_cluster_cfg);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer_name);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model_identifier);

    // identify cluster
    esp_zb_identify_cluster_cfg_t identify_cluster_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *esp_zb_identify_cluster = esp_zb_identify_cluster_create(&identify_cluster_cfg);

    // binary input cluster
    esp_zb_binary_input_cluster_cfg_t binary_input_cfg = {
        .out_of_service = ESP_ZB_ZCL_BINARY_INPUT_OUT_OF_SERVICE_DEFAULT_VALUE,
        .status_flags = ESP_ZB_ZCL_BINARY_INPUT_STATUS_FLAG_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *esp_zb_binary_input_cluster = esp_zb_binary_input_cluster_create(&binary_input_cfg);
    uint8_t present_value = 0;
    esp_zb_binary_input_cluster_add_attr(esp_zb_binary_input_cluster, ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID, &present_value);
    char description[16];
    esp_zb_set_zcl_string(description, "a simple button");
    esp_zb_binary_input_cluster_add_attr(esp_zb_binary_input_cluster, ESP_ZB_ZCL_ATTR_BINARY_INPUT_DESCRIPTION_ID, description);

    // 2- cluster list creation
    esp_zb_cluster_list_t *esp_zb_cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(esp_zb_cluster_list, esp_zb_basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(esp_zb_cluster_list, esp_zb_identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_binary_input_cluster(esp_zb_cluster_list, esp_zb_binary_input_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    return esp_zb_cluster_list;
}

esp_zb_cluster_list_t *esp_zb_create_light_cluster_list()
{
    // using default config
    esp_zb_color_dimmable_light_cfg_t light_cfg = ESP_ZB_DEFAULT_COLOR_DIMMABLE_LIGHT_CONFIG();
    // esp_zb_color_dimmable_light_clusters_create creates all the needed clusters
    esp_zb_cluster_list_t *esp_zb_cluster_list = esp_zb_color_dimmable_light_clusters_create(&light_cfg);

    return esp_zb_cluster_list;
}

static void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    // 1- cluster list creation
    esp_zb_cluster_list_t *esp_zb_button_cluster_list = esp_zb_create_button_cluster_list();
    esp_zb_cluster_list_t *esp_zb_color_dimmable_light_cluster_list = esp_zb_create_light_cluster_list();

    // 2- endpoint list creation
    esp_zb_ep_list_t *esp_zb_ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_button_cluster_list, HA_BUTTON_ENDPOINT, ESP_ZB_AF_HA_PROFILE_ID, ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID);
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_color_dimmable_light_cluster_list, HA_LIGHT_ENDPOINT, ESP_ZB_AF_HA_PROFILE_ID,
                          ESP_ZB_HA_COLOR_DIMMABLE_LIGHT_DEVICE_ID);

    // 3- device creation
    esp_zb_device_register(esp_zb_ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);

    // 4- run the thing
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_main_loop_iteration();
}

// main run
void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    // 1- zigbee conf
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    // 2- ins & outs init
    gpio_set_direction(GPIO_NUM_12, GPIO_MODE_INPUT);
    light_driver_init(LIGHT_DEFAULT_OFF);

    // 3- zigbee task
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}

// callback to restart the commissioning process when something goes wrong
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

// zigbee signal handler
// handles the zigbee stack signals and if the device joins a network
// it starts any needed tasks
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type)
    {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK)
        {
            ESP_LOGI(TAG, "Start network steering");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        }
        else
        {
            /* commissioning failed */
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK)
        {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel());
            xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);
        }
        else
        {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}