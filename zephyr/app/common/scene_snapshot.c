#include "imu_sample.h"
#include "scene_snapshot.h"

static struct vec3 apply_zoom(const float zoom[3], struct vec3 v)
{
	return vec3_make(v.x * zoom[0], v.y * zoom[1], v.z * zoom[2]);
}

static void project_axis_line(const float zoom[3], const struct mat3 *rot, struct vec3 dir,
			      float scale, int screen_w, int screen_h,
			      const struct camera3d *cam, struct scene_axis_line *out)
{
	const struct vec3 end =
		mat3_transform(rot, apply_zoom(zoom, vec3_scale(vec3_norm(dir), scale)));

	out->p0 = projection_project(vec3_make(0, 0, 2.5f), screen_w, screen_h, cam);
	out->p1 = projection_project(vec3_make(end.x, end.y, 2.5f + end.z), screen_w, screen_h, cam);
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

	project_axis_line(zoom, rot, vec3_make(1, 0, 0), 1.2f, screen_w, screen_h, &cam,
			  &snap.axes[0]);
	project_axis_line(zoom, rot, vec3_make(0, 1, 0), 1.2f, screen_w, screen_h, &cam,
			  &snap.axes[1]);
	project_axis_line(zoom, rot, vec3_make(0, 0, 1), 1.2f, screen_w, screen_h, &cam,
			  &snap.axes[2]);

	for (int i = 0; i < 8; i++) {
		struct vec3 p = mat3_transform(rot, apply_zoom(zoom, unit_corners[i]));

		p.z += 2.0f;
		snap.corners[i] = projection_project(p, screen_w, screen_h, &cam);
	}

	if (sample != NULL) {
		struct vec3 probe = vec3_scale(world_linear_accel(sample, rot), 0.45f);

		probe.z += 2.0f;
		const struct vec2 touch =
			projection_project(probe, screen_w, screen_h, &cam);

		snap.footer_unproject =
			projection_unproject(touch, probe.z, screen_w, screen_h, &cam);
	}

	return snap;
}
