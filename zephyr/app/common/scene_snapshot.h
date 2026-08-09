#pragma once

#include "imu_math.h"
#include "projection.h"

struct scene_axis_line {
	struct vec2 p0;
	struct vec2 p1;
};

struct scene_snapshot {
	int screen_w;
	int screen_h;
	int center_x;
	int center_y;
	struct scene_axis_line axes[3];
	struct vec2 corners[8];
	struct vec3 footer_unproject;
	float walk_distance_m;
};

#define SCENE_ZOOM_DEFAULT 0.75f

struct scene_snapshot scene_snapshot_build(int screen_w, int screen_h, const float zoom[3],
					 const struct mat3 *rot, const struct imu_sample *sample,
					 float walk_distance_m);
