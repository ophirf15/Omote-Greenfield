# OMOTE Hardware Rev 1–4 (ESP32)

Reference: [OMOTE-Hardware](https://github.com/OMOTE-Community/OMOTE-Hardware/)

| Function | GPIO |
|----------|------|
| I2C SDA / SCL | 19 / 22 |
| LCD backlight | 4 |
| LCD EN | 10 |
| LCD CS | 5 |
| LCD DC | 9 |
| LCD MOSI | 23 |
| LCD SCK | 18 |
| Touch FT5x06 | I2C 0x38 |
| IR LED TX | 33 |
| IR RX | 15 |
| IR RX power | 25 |
| Keypad outputs | 32, 26, 27, 14, 12 |
| Keypad inputs | 37, 38, 39, 34, 35 |
| Power button (above LCD) | GPIO 0 (active low; global key `P`) |
| User LED | 2 |
| Battery (rev4) | MAX17048 on I2C |
| Charger status | 21 |

Display: 240×320 ILI9341 (SPI). Keypad: 5×5 matrix.
