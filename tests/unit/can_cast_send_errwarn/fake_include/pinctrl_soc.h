/* Host stand-in so <zephyr/drivers/pinctrl.h> compiles; no pin states are used. */
#include <stdint.h>
typedef uint32_t pinctrl_soc_pin_t;
#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop) {0}
