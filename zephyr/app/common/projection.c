#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "projection.h"

struct vec2 projection_project(struct vec3 point, int screen_w, int screen_h,
			       const struct camera3d *cam)
{
	const float f = 1.0f / tanf((cam->fov_deg * 0.5f) * ((float)M_PI / 180.0f));
	const float z = fmaxf(point.z, cam->near_plane);
	const float sx = (point.x * f / (z * cam->aspect)) * (screen_w * 0.5f) + screen_w * 0.5f;
	const float sy = (-point.y * f / z) * (screen_h * 0.5f) + screen_h * 0.5f;

	return (struct vec2){ sx, sy };
}

struct vec3 projection_unproject(struct vec2 screen, float depth, int screen_w, int screen_h,
				 const struct camera3d *cam)
{
	const float f = 1.0f / tanf((cam->fov_deg * 0.5f) * ((float)M_PI / 180.0f));
	const float z = fmaxf(depth, cam->near_plane);
	const float nx = (screen.x - screen_w * 0.5f) / (screen_w * 0.5f);
	const float ny = -(screen.y - screen_h * 0.5f) / (screen_h * 0.5f);

	return vec3_make(nx * z * cam->aspect / f, ny * z / f, z);
}
