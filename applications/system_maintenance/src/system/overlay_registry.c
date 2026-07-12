#include "overlay_registry.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const REQ_I2C0[] = {"i2c0"};
static const char *const REQ_I2S[] = {"i2s0_pa", "i2s0_pe"};
static const char *const CON_ADC_PA123[] = {"adc_pa1", "uart1", "i2s0_pa", "i2s0_pe"};
static const char *const CON_ADC_PA1[] = {"adc_pa123", "i2s0_pa", "i2s0_pe"};
static const char *const CON_UART1[] = {"adc_pa123", "i2s0_pa"};
static const char *const CON_I2S_PA[] = {"i2s0_pe", "adc_pa123", "adc_pa1", "uart1"};
static const char *const CON_I2S_PE[] = {"i2s0_pa", "adc_pa123", "adc_pa1"};
static const char *const CON_UART2[] = {"spi1"};
static const char *const CON_SPI1[] = {"uart2"};

#define ITEM(identifier, category_value, title, help, min_rank, max_rank) \
    {identifier, category_value, title, help, min_rank, max_rank, NULL, 0U, \
     OVERLAY_DEPENDENCY_ALL, NULL, 0U}

static const overlay_registry_item_t ITEMS[] = {
    {"adc_pa123", OVERLAY_CATEGORY_INTERFACE, "三路 ADC",
     "将 PA1、PA2、PA3 用作 ADC 引脚。", -1, -1, NULL, 0U,
     OVERLAY_DEPENDENCY_ALL, CON_ADC_PA123, 4U},
    {"adc_pa1", OVERLAY_CATEGORY_INTERFACE, "单路 ADC",
     "将 PA1 用作 ADC 引脚。", -1, -1, NULL, 0U,
     OVERLAY_DEPENDENCY_ALL, CON_ADC_PA1, 3U},
    ITEM("i2c0", OVERLAY_CATEGORY_INTERFACE, "I2C 0",
         "启用 I2C0，使用 PD0/PD12。", -1, -1),
    {"i2s0_pa", OVERLAY_CATEGORY_INTERFACE, "I2S 0（PA）",
     "启用 I2S0，使用 PA1/PA2/PA3/PE3；仅 0.5 及以上硬件可用。",
     2, -1, NULL, 0U, OVERLAY_DEPENDENCY_ALL, CON_I2S_PA, 4U},
    {"i2s0_pe", OVERLAY_CATEGORY_INTERFACE, "I2S 0（PE）",
     "启用 I2S0，使用 PA1/PE5/PE6/PE3；仅 0.5 及以上硬件可用。",
     2, -1, NULL, 0U, OVERLAY_DEPENDENCY_ALL, CON_I2S_PE, 3U},
    {"spi1", OVERLAY_CATEGORY_INTERFACE, "SPI 1",
     "启用 SPI1，使用 PE7/PE8/PE9/PE10。", -1, -1, NULL, 0U,
     OVERLAY_DEPENDENCY_ALL, CON_SPI1, 1U},
    {"uart1", OVERLAY_CATEGORY_INTERFACE, "UART 1",
     "启用 UART1，使用 PA2/PA3。", -1, -1, NULL, 0U,
     OVERLAY_DEPENDENCY_ALL, CON_UART1, 2U},
    {"uart2", OVERLAY_CATEGORY_INTERFACE, "UART 2",
     "启用 UART2，使用 PA7/PA8。", -1, -1, NULL, 0U,
     OVERLAY_DEPENDENCY_ALL, CON_UART2, 1U},
    ITEM("usbhost", OVERLAY_CATEGORY_INTERFACE, "USB 主机",
         "将 USB 控制器切换为主机模式。", -1, -1),
    ITEM("usbhs", OVERLAY_CATEGORY_INTERFACE, "USB 高速",
         "启用 USB 2.0 High-Speed 模式。", -1, -1),
    {"cardkb", OVERLAY_CATEGORY_EXTENSION, "CardKB 键盘",
     "启用 M5Stack CardKB 键盘支持，需要 I2C0。", -1, -1,
     REQ_I2C0, 1U, OVERLAY_DEPENDENCY_ALL, NULL, 0U},
    {"lsm6ds3_pre0.4", OVERLAY_CATEGORY_EXTENSION, "板载 LSM6DS3",
     "启用 0.4 及以前硬件的板载 IMU，需要 I2C0。", -1, 1,
     REQ_I2C0, 1U, OVERLAY_DEPENDENCY_ALL, NULL, 0U},
    {"es8311_sound", OVERLAY_CATEGORY_EXTENSION, "ES8311 声卡",
     "启用 ES8311 音频编解码器，需要选择一种 I2S0 路由；仅 0.5 及以上可用。",
     2, -1, REQ_I2S, 2U, OVERLAY_DEPENDENCY_ANY, NULL, 0U}
};

#undef ITEM

const overlay_registry_t *overlay_registry_get(void)
{
    static const overlay_registry_t registry = {
        ITEMS, sizeof(ITEMS) / sizeof(ITEMS[0])
    };
    return &registry;
}

const overlay_registry_item_t *overlay_registry_find(const char *id)
{
    const overlay_registry_t *registry = overlay_registry_get();
    size_t index;
    if (id == NULL) {
        return NULL;
    }
    for (index = 0U; index < registry->count; ++index) {
        if (strcmp(registry->items[index].id, id) == 0) {
            return &registry->items[index];
        }
    }
    return NULL;
}

int overlay_revision_rank(const char *device_revision)
{
    const char *number;
    char *end;
    double revision;
    if (device_revision == NULL) {
        return -1;
    }
    number = device_revision;
    while (*number != '\0' && !isdigit((unsigned char)*number)) {
        ++number;
    }
    if (*number == '\0') {
        return -1;
    }
    revision = strtod(number, &end);
    if (end == number) {
        return -1;
    }
    if (revision >= 0.6) return 3;
    if (revision >= 0.5) return 2;
    if (revision >= 0.3) return 1;
    if (revision >= 0.2) return 0;
    return -1;
}

bool overlay_registry_available(const overlay_registry_item_t *item,
                                const char *device_revision)
{
    int rank;
    if (item == NULL) {
        return false;
    }
    if (item->min_revision_rank < 0 && item->max_revision_rank < 0) {
        return true;
    }
    rank = overlay_revision_rank(device_revision);
    if (rank < 0) {
        return false;
    }
    return (item->min_revision_rank < 0 || rank >= item->min_revision_rank) &&
           (item->max_revision_rank < 0 || rank <= item->max_revision_rank);
}
