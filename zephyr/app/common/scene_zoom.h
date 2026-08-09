#pragma once

#include "imu_sample.h"

#define SCENE_ZOOM_MIN         0.05f
#define SCENE_ZOOM_MAX         2.00f
#define SCENE_ZOOM_DEFAULT     0.75f
#define SCENE_ZOOM_MOTION_MIN  0.25f
#define SCENE_ZOOM_STEP        0.02f
#define SCENE_ZOOM_SMOOTH      0.18f
#define SCENE_MOTION_GYRO_FULL_DPS 120.0f

void scene_zoom_init(void);
void scene_zoom_tick(const struct imu_sample *sample);
const float *scene_zoom_current(void);
