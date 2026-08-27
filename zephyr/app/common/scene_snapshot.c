#include "imu_sample.h"
#include "scene_snapshot.h"
#include "scene_zoom.h"

#include <math.h>
#include <stdbool.h>

#define CAM_Z_SAFE 0.75f

static float sane_zoom(float z)
{
	if (!isfinite(z) || z < SCENE_ZOOM_MIN || z > SCENE_ZOOM_MAX) {
		return SCENE_ZOOM_DEFAULT;
	}
	return z;
}

static struct vec3 apply_zoom(const float zoom[3], struct vec3 v)
{
	const float zx = zoom != NULL ? sane_zoom(zoom[0]) : SCENE_ZOOM_DEFAULT;
	const float zy = zoom != NULL ? sane_zoom(zoom[1]) : SCENE_ZOOM_DEFAULT;
	const float zz = zoom != NULL ? sane_zoom(zoom[2]) : SCENE_ZOOM_DEFAULT;

	return vec3_make(v.x * zx, v.y * zy, v.z * zz);
}

/* Clip the segment to z >= CAM_Z_SAFE so perspective cannot explode to the
 * panel corner (the usual stall: one axis from center to upper-left, no cube). */
static bool clip_near_z(struct vec3 *a, struct vec3 *b)
{
	const bool a_ok = isfinite(a->x) && isfinite(a->y) && isfinite(a->z) && a->z >= CAM_Z_SAFE;
	const bool b_ok = isfinite(b->x) && isfinite(b->y) && isfinite(b->z) && b->z >= CAM_Z_SAFE;

	if (a_ok && b_ok) {
		return true;
	}
	if (!a_ok && !b_ok) {
		return false;
	}

	struct vec3 *outp = a_ok ? b : a;
	const struct vec3 *inp = a_ok ? a : b;
	const float denom = outp->z - inp->z;

	if (!isfinite(denom) || fabsf(denom) < 1e-6f) {
		return false;
	}

	const float t = (CAM_Z_SAFE - inp->z) / denom;

	outp->x = inp->x + t * (outp->x - inp->x);
	outp->y = inp->y + t * (outp->y - inp->y);
	outp->z = CAM_Z_SAFE;
	return isfinite(outp->x) && isfinite(outp->y);
}

static bool project_line(struct vec3 a, struct vec3 b, int screen_w, int screen_h,
			 const struct camera3d *cam, struct vec2 *p0, struct vec2 *p1)
{
	if (!clip_near_z(&a, &b)) {
		return false;
	}

	*p0 = projection_project(a, screen_w, screen_h, cam);
	*p1 = projection_project(b, screen_w, screen_h, cam);
	return isfinite(p0->x) && isfinite(p0->y) && isfinite(p1->x) && isfinite(p1->y);
}

static void project_axis_line(const float zoom[3], const struct mat3 *rot, struct vec3 dir,
			      float scale, int screen_w, int screen_h,
			      const struct camera3d *cam, struct scene_axis_line *out)
{
	const struct vec3 origin = vec3_make(0, 0, 2.5f);
	const struct vec3 end =
		mat3_transform(rot, apply_zoom(zoom, vec3_scale(vec3_norm(dir), scale)));
	const struct vec3 tip = vec3_make(end.x, end.y, 2.5f + end.z);

	if (!project_line(origin, tip, screen_w, screen_h, cam, &out->p0, &out->p1)) {
		out->p0 = projection_project(origin, screen_w, screen_h, cam);
		out->p1 = out->p0;
	}
}

static struct vec3 world_linear_accel(const struct imu_sample *sample, const struct mat3 *rot)
{
	struct vec3 body = vec3_make(sample->ax, sample->ay, sample->az);
	struct vec3 world = mat3_transform(rot, body);

	world.z -= 1.0f;
	return world;
}

struct scene_snapshot scene_snapshot_build(int screen_w, int screen_h, const float zoom[3],
					   const struct mat3 *rot, const struct imu_sample *sample,
					   float walk_distance_m)
{
	struct scene_snapshot snap = {
		.screen_w = screen_w,
		.screen_h = screen_h,
		.center_x = screen_w / 2,
		.center_y = screen_h / 2 + 10,
		.walk_distance_m = walk_distance_m,
	};
	struct camera3d cam = {
		.fov_deg = 45.0f,
		.near_plane = 0.1f,
		.aspect = (float)screen_w / (float)screen_h,
	};
	const struct vec3 unit_corners[8] = {
		{ -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, -0.5f },
		{ -0.5f, 0.5f, -0.5f },  { -0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f },
		{ 0.5f, 0.5f, 0.5f },    { -0.5f, 0.5f, 0.5f },
	};

	const struct mat3 ident = mat3_identity();
	const struct mat3 *R = rot;

	if (R == NULL || !isfinite(R->m[0][0]) || !isfinite(R->m[1][1]) ||
	    !isfinite(R->m[2][2])) {
		R = &ident;
	}

	project_axis_line(zoom, R, vec3_make(1, 0, 0), 1.2f, screen_w, screen_h, &cam,
			  &snap.axes[0]);
	project_axis_line(zoom, R, vec3_make(0, 1, 0), 1.2f, screen_w, screen_h, &cam,
			  &snap.axes[1]);
	project_axis_line(zoom, R, vec3_make(0, 0, 1), 1.2f, screen_w, screen_h, &cam,
			  &snap.axes[2]);

	for (int i = 0; i < 8; i++) {
		struct vec3 mid = vec3_make(0, 0, 2.0f);
		struct vec3 p = mat3_transform(R, apply_zoom(zoom, unit_corners[i]));

		p.z += 2.0f;
		if (!clip_near_z(&mid, &p)) {
			snap.corners[i] = (struct vec2){ (float)snap.center_x, (float)snap.center_y };
		} else {
			snap.corners[i] = projection_project(p, screen_w, screen_h, &cam);
			if (!isfinite(snap.corners[i].x) || !isfinite(snap.corners[i].y)) {
				snap.corners[i] = (struct vec2){ (float)snap.center_x,
								 (float)snap.center_y };
			}
		}
	}

	if (sample != NULL) {
		struct vec3 probe = vec3_scale(world_linear_accel(sample, R), 0.45f);

		probe.z += 2.0f;
		const struct vec2 touch =
			projection_project(probe, screen_w, screen_h, &cam);

		snap.footer_unproject =
			projection_unproject(touch, probe.z, screen_w, screen_h, &cam);
	}

	return snap;
}
