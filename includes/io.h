/* Sources:
    https://rpi4os.com/part3-helloworld/
*/

#pragma once

void mmio_write(long reg, unsigned int val);
unsigned int mmio_read(long reg) ;

unsigned int gpio_set(unsigned int pin_number, unsigned int value);
unsigned int gpio_clear(unsigned int pin_number, unsigned int value);
unsigned int gpio_pull(unsigned int pin_number, unsigned int value);
unsigned int gpio_function(unsigned int pin_number, unsigned int value);

void uart_init();
void uart_writeText();