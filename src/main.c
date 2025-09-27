#include <zephyr/device.h>
#include <zephyr/drivers/auxdisplay.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

/* Thread stack sizes */
#define BUTTON_THREAD_STACK_SIZE 512
#define DISPLAY_THREAD_STACK_SIZE 512
#define RNG_THREAD_STACK_SIZE 1024

/* Thread priorities */
#define BUTTON_THREAD_PRIORITY 7
#define DISPLAY_THREAD_PRIORITY 8
#define RNG_THREAD_PRIORITY 9

/* Animation parameters */
#define ANIMATION_DURATION_MS 2000
#define FAST_UPDATE_MS 50
#define MEDIUM_UPDATE_MS 100
#define SLOW_UPDATE_MS 200
#define FINAL_UPDATE_MS 500

/* Device handles */
static const struct device* entropy_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_entropy));
static const struct device* display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_auxdisplay));
static const struct gpio_dt_spec button_spec = GPIO_DT_SPEC_GET(DT_ALIAS(key), gpios);

/* Thread stacks */
K_THREAD_STACK_DEFINE(button_thread_stack, BUTTON_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(display_thread_stack, DISPLAY_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(rng_thread_stack, RNG_THREAD_STACK_SIZE);

/* Thread control blocks */
static struct k_thread button_thread_data;
static struct k_thread display_thread_data;
static struct k_thread rng_thread_data;

/* Synchronization objects */
K_SEM_DEFINE(dice_roll_sem, 0, 1);
K_SEM_DEFINE(display_update_sem, 0, 1);
K_MUTEX_DEFINE(dice_value_mutex);

/* Global state */
static volatile bool rolling = false;
static volatile int current_dice_value = 1;
static volatile int final_dice_value = 0;

/* Button interrupt handler */
static struct gpio_callback button_cb_data;

int get_true_random_dice(void) {
  uint32_t random_value;
  int ret;

  if (!device_is_ready(entropy_dev)) {
    return (sys_rand32_get() % 6) + 1;
  }

  ret = entropy_get_entropy(entropy_dev, (uint8_t*)&random_value, sizeof(random_value));
  if (ret < 0) {
    return (sys_rand32_get() % 6) + 1;
  }

  return (random_value % 6) + 1;
}

void update_display(int value) {
  char display_str[2];
  snprintf(display_str, sizeof(display_str), "%d", value);
  auxdisplay_write(display_dev, display_str, strlen(display_str));
}

void button_pressed_handler(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
  if (!rolling) {
    k_sem_give(&dice_roll_sem);
  }
}

/* Button monitoring thread */
void button_thread_entry(void* arg1, void* arg2, void* arg3) {
  while (1) {
    /* Wait for button press */
    k_sem_take(&dice_roll_sem, K_FOREVER);

    if (!rolling) {
      k_mutex_lock(&dice_value_mutex, K_FOREVER);
      rolling = true;
      k_mutex_unlock(&dice_value_mutex);

      /* Signal display thread to start animation */
      k_sem_give(&display_update_sem);
    }

    /* Debounce delay */
    k_msleep(100);
  }
}

/* RNG thread - generates random numbers continuously during roll */
void rng_thread_entry(void* arg1, void* arg2, void* arg3) {
  while (1) {
    k_mutex_lock(&dice_value_mutex, K_FOREVER);
    if (rolling) {
      current_dice_value = get_true_random_dice();
    }
    k_mutex_unlock(&dice_value_mutex);

    /* Generate numbers at high frequency when rolling */
    if (rolling) {
      k_msleep(10);
    } else {
      k_msleep(100); /* Idle state */
    }
  }
}

/* Display animation thread */
void display_thread_entry(void* arg1, void* arg2, void* arg3) {
  uint32_t elapsed_time;
  uint32_t start_time;
  uint32_t update_interval;

  update_display(0);

  while (1) {
    /* Wait for roll initiation */
    k_sem_take(&display_update_sem, K_FOREVER);

    start_time = k_uptime_get_32();
    elapsed_time = 0;

    /* Animation phases with decreasing speed */
    while (elapsed_time < ANIMATION_DURATION_MS) {
      elapsed_time = k_uptime_get_32() - start_time;

      /* Determine update speed based on elapsed time */
      if (elapsed_time < 500) {
        update_interval = FAST_UPDATE_MS;
      } else if (elapsed_time < 1000) {
        update_interval = MEDIUM_UPDATE_MS;
      } else if (elapsed_time < 1500) {
        update_interval = SLOW_UPDATE_MS;
      } else {
        update_interval = FINAL_UPDATE_MS;
      }

      /* Update display with current random value */
      k_mutex_lock(&dice_value_mutex, K_FOREVER);
      update_display(current_dice_value);
      final_dice_value = current_dice_value;
      k_mutex_unlock(&dice_value_mutex);

      k_msleep(update_interval);
    }

    /* Generate final result, one last time */
    k_mutex_lock(&dice_value_mutex, K_FOREVER);
    final_dice_value = get_true_random_dice();
    update_display(final_dice_value);
    k_mutex_unlock(&dice_value_mutex);

    k_mutex_lock(&dice_value_mutex, K_FOREVER);
    rolling = false;
    k_mutex_unlock(&dice_value_mutex);

    k_msleep(1000);
  }
}

int main(void) {
  int ret;

  /* Initialize display device */
  if (!device_is_ready(display_dev)) {
    return -ENODEV;
  }

  ret = auxdisplay_display_on(display_dev);
  if (ret < 0) {
    return ret;
  }

  auxdisplay_brightness_set(display_dev, 1);

  /* Initialize button GPIO */
  if (!gpio_is_ready_dt(&button_spec)) {
    return -ENODEV;
  }

  ret = gpio_pin_configure_dt(&button_spec, GPIO_INPUT);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_interrupt_configure_dt(&button_spec, GPIO_INT_EDGE_TO_ACTIVE);
  if (ret < 0) {
    return ret;
  }

  gpio_init_callback(&button_cb_data, button_pressed_handler, BIT(button_spec.pin));
  gpio_add_callback(button_spec.port, &button_cb_data);

  k_thread_create(&button_thread_data,
                  button_thread_stack,
                  K_THREAD_STACK_SIZEOF(button_thread_stack),
                  button_thread_entry,
                  NULL,
                  NULL,
                  NULL,
                  BUTTON_THREAD_PRIORITY,
                  0,
                  K_NO_WAIT);

  k_thread_create(&display_thread_data,
                  display_thread_stack,
                  K_THREAD_STACK_SIZEOF(display_thread_stack),
                  display_thread_entry,
                  NULL,
                  NULL,
                  NULL,
                  DISPLAY_THREAD_PRIORITY,
                  0,
                  K_NO_WAIT);

  k_thread_create(&rng_thread_data,
                  rng_thread_stack,
                  K_THREAD_STACK_SIZEOF(rng_thread_stack),
                  rng_thread_entry,
                  NULL,
                  NULL,
                  NULL,
                  RNG_THREAD_PRIORITY,
                  0,
                  K_NO_WAIT);

  k_thread_name_set(&button_thread_data, "button");
  k_thread_name_set(&display_thread_data, "display");
  k_thread_name_set(&rng_thread_data, "rng");

  while (1) {
    k_sleep(K_FOREVER);
  }

  return 0;
}
