#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(board, CONFIG_LOG_DEFAULT_LEVEL);

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define MODEM_GPIO_NODE DT_NODELABEL(modem_control)
#define DELAY_MODEM_PWR_EN K_MSEC(CONFIG_BOARD_MODEM_POWER_EN_DELAY_MS)
#define DELAY_MODEM_BOOT                                                                           \
	K_MSEC(CONFIG_BOARD_MODEM_BOOT_DELAY_MS + CONFIG_BOARD_MODEM_POWER_EN_DELAY_MS)
#define DELAY_MODEM_PWR_ON K_MSEC(500)

static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(LED2_NODE, gpios);
static const struct gpio_dt_spec modem_pwr_en_gpio =
	GPIO_DT_SPEC_GET_OR(MODEM_GPIO_NODE, dcdc_en_gpios, {});
static const struct gpio_dt_spec modem_powerkey_gpio =
	GPIO_DT_SPEC_GET(MODEM_GPIO_NODE, modem_powerkey_gpios);
static const struct gpio_dt_spec modem_rst_gpio =
	GPIO_DT_SPEC_GET(MODEM_GPIO_NODE, modem_reset_gpios);
static const struct gpio_dt_spec modem_boot_gpio =
	GPIO_DT_SPEC_GET_OR(MODEM_GPIO_NODE, modem_usb_boot_gpios, {});

static int set_current_limit(void)
{
	struct sensor_value limit = {
		.val1 = 1,
		.val2 = 500000,
	};

	if (!device_is_ready(charger)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	int ret = sensor_attr_set(charger, SENSOR_CHAN_CURRENT, SENSOR_ATTR_CONFIGURATION, &limit);

	if (ret < 0) {
		LOG_ERR("Failed to set current limit: %d", ret);
	}
	return ret;
}
SYS_INIT(set_current_limit, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);

static void modem_power_enable(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	if (!gpio_is_ready_dt(&modem_pwr_en_gpio)) {
		LOG_ERR("Modem power enable GPIO device not ready");
		return;
	}

	ret = gpio_pin_configure_dt(&modem_pwr_en_gpio, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure modem power enable GPIO: %d", ret);
		return;
	}
}
static K_WORK_DELAYABLE_DEFINE(modem_pwr_en_work, modem_power_enable);

static int init_modem_power_enable(void)
{
	k_work_schedule(&modem_pwr_en_work, DELAY_MODEM_PWR_EN);

	return 0;
}
SYS_INIT(init_modem_power_enable, APPLICATION, 0);

static int gpio_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&modem_rst_gpio) || !gpio_is_ready_dt(&modem_powerkey_gpio)) {
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&led_blue) || !gpio_is_ready_dt(&led_red) ||
	    !gpio_is_ready_dt(&led_green)) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&modem_rst_gpio, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}
	err = gpio_pin_configure_dt(&modem_powerkey_gpio, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}
	if (gpio_is_ready_dt(&modem_boot_gpio)) {
		err = gpio_pin_configure_dt(&modem_boot_gpio, GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}
	err = gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}
	err = gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}
	err = gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}

	return 0;
}

SYS_INIT(gpio_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

#if CONFIG_BOARD_REVISION_V0_2 || CONFIG_BOARD_REVISION_V0_3
static int set_modem_uart_bypass(void)
{
	const struct gpio_dt_spec uart_bypass_gpio =
		GPIO_DT_SPEC_GET(MODEM_GPIO_NODE, vcom1_ctl_gpios);
	int ret;

	if (!gpio_is_ready_dt(&uart_bypass_gpio)) {
		LOG_ERR("Modem bypass GPIO device not ready");
		return -ENODEV;
	}

	gpio_flags_t flags = (IS_ENABLED(CONFIG_BOARD_MODEM_UART_BYPASS) ? GPIO_OUTPUT_ACTIVE
									 : GPIO_OUTPUT_INACTIVE);

	ret = gpio_pin_configure_dt(&uart_bypass_gpio, flags);
	if (ret < 0) {
		LOG_ERR("Failed to configure modem bypass GPIO: %d", ret);
		return ret;
	}

	return ret;
}

SYS_INIT(set_modem_uart_bypass, APPLICATION, 0);
#endif /* BOARD_REVISION_V0_2 || CONFIG_BOARD_REVISION_V0_3 */

static void modem_boot_work(struct k_work *work);

struct {
	enum {
		ACTIVATE_PWRKEY,
		DEACTIVATE_PWRKEY,
		DONE
	} state;
	struct k_work_delayable dwork;
} static nrf93_pwr_state_data = {
	.state = ACTIVATE_PWRKEY,
	.dwork = Z_WORK_DELAYABLE_INITIALIZER(modem_boot_work),
};

static void modem_boot_work(struct k_work *work)
{
	int ret = 0;

	switch (nrf93_pwr_state_data.state) {
	case ACTIVATE_PWRKEY:
		ret = gpio_pin_set_dt(&modem_powerkey_gpio, 1);
		nrf93_pwr_state_data.state = DEACTIVATE_PWRKEY;
		k_work_reschedule(&nrf93_pwr_state_data.dwork, DELAY_MODEM_PWR_ON);
		break;
	case DEACTIVATE_PWRKEY:
		ret = gpio_pin_set_dt(&modem_powerkey_gpio, 0);
		nrf93_pwr_state_data.state = DONE;
		break;
	default:
		return;
	}

	if (ret < 0) {
		LOG_ERR("Failed to set modem GPIO: %d", ret);
	}
}

static int power_modem_on_boot(void)
{
	if (IS_ENABLED(CONFIG_BOARD_POWER_MODEM_ON_BOOT)) {
		k_work_schedule(&nrf93_pwr_state_data.dwork, DELAY_MODEM_BOOT);
	}
	return 0;
}
SYS_INIT(power_modem_on_boot, APPLICATION, 1);
