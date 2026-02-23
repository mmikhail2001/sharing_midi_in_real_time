// midi.h
#pragma once

#define MIDI_MESSAGE_LENGTH 		4

extern QueueHandle_t midi_queue;

// MIDI message structure: 4 bytes MIDI + ns timestamp
typedef struct {
    uint8_t data[MIDI_MESSAGE_LENGTH];  // Raw MIDI bytes
    int64_t local_ns;
    int64_t boot_ns;
} midi_message_t;

void class_driver_task(void *arg);
void host_lib_daemon_task(void *arg);
