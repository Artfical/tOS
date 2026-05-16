#include "midi.h"
#include "io.h"
int midi_init(midi_device_t *dev)
{
    dev->base = MPU401_STATUS;
    outb(dev->base, MPU401_CMD_RESET);
    for (volatile int i = 0; i < 100000; i++);
    outb(dev->base, MPU401_CMD_UART);
    dev->present = 1;
    return 0;
}
void midi_write(midi_device_t *dev, uint8_t byte)
{
    for (volatile int i = 0; i < 100000; i++)
        if (!(inb(dev->base) & 0x40)) break;
    outb(dev->base - 1, byte);
}
void midi_note_on(midi_device_t *dev, uint8_t channel, uint8_t note, uint8_t velocity)
{
    midi_write(dev, MIDI_NOTE_ON | (channel & 0x0F));
    midi_write(dev, note & 0x7F);
    midi_write(dev, velocity & 0x7F);
}
void midi_note_off(midi_device_t *dev, uint8_t channel, uint8_t note, uint8_t velocity)
{
    midi_write(dev, MIDI_NOTE_OFF | (channel & 0x0F));
    midi_write(dev, note & 0x7F);
    midi_write(dev, velocity & 0x7F);
}
