#ifndef MIDI_H
#define MIDI_H
#include <stdint.h>
#define MPU401_DATA 0x330
#define MPU401_STATUS 0x331
#define MPU401_CMD 0x331
#define MPU401_CMD_UART 0x3F
#define MPU401_CMD_RESET 0xFF
#define MIDI_NOTE_OFF 0x80
#define MIDI_NOTE_ON 0x90
#define MIDI_POLY_AFTER 0xA0
#define MIDI_CTRL_CHANGE 0xB0
#define MIDI_PROG_CHANGE 0xC0
#define MIDI_CHAN_AFTER 0xD0
#define MIDI_PITCH_BEND 0xE0
typedef struct {
    int present;
    uint16_t base;
} midi_device_t;
int midi_init(midi_device_t *dev);
void midi_write(midi_device_t *dev, uint8_t byte);
void midi_note_on(midi_device_t *dev, uint8_t channel, uint8_t note, uint8_t velocity);
void midi_note_off(midi_device_t *dev, uint8_t channel, uint8_t note, uint8_t velocity);
#endif
