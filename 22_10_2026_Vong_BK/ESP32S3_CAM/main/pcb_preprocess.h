#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float corners[4][2]; /* top-left, top-right, bottom-right, bottom-left */
    float angle_deg;
    int foreground_pixels;
} pcb_box_t;

/* Detect a green or yellow PCB in a decoded RGB888 640x480 camera frame. */
bool pcb_detect_rgb(const uint8_t *rgb, int width, int height, pcb_box_t *box);

/* Warp the detected PCB into a centered 640x448 RGB888 inspection image. */
void pcb_normalize_rgb(const uint8_t *source, int source_width, int source_height,
                       const pcb_box_t *box, uint8_t *destination,
                       int destination_width, int destination_height);
