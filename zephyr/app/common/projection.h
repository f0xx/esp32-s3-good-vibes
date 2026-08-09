#pragma once

#include "imu_math.h"

struct vec2 projection_project(struct vec3 point, int screen_w, int screen_h,
			       const struct camera3d *cam);
struct vec3 projection_unproject(struct vec2 screen, float depth, int screen_w, int screen_h,
				 const struct camera3d *cam);
