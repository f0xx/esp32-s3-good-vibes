#pragma once

#include <math.h>
#include <stdint.h>

struct vec2 {
	float x;
	float y;
};

struct vec3 {
	float x;
	float y;
	float z;
};

struct mat3 {
	float m[3][3];
};

struct camera3d {
	float fov_deg;
	float near_plane;
	float aspect;
};

static inline struct vec3 vec3_make(float x, float y, float z)
{
	return (struct vec3){ x, y, z };
}

static inline float vec3_dot(struct vec3 a, struct vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline float vec3_len(struct vec3 v)
{
	return sqrtf(vec3_dot(v, v));
}

static inline struct vec3 vec3_norm(struct vec3 v)
{
	const float len = vec3_len(v);

	if (len < 1e-6f) {
		return vec3_make(0.0f, 0.0f, 0.0f);
	}
	return vec3_make(v.x / len, v.y / len, v.z / len);
}

static inline struct vec3 vec3_scale(struct vec3 v, float s)
{
	return vec3_make(v.x * s, v.y * s, v.z * s);
}

static inline struct vec3 vec3_add(struct vec3 a, struct vec3 b)
{
	return vec3_make(a.x + b.x, a.y + b.y, a.z + b.z);
}

static inline struct vec3 mat3_transform(const struct mat3 *m, struct vec3 v)
{
	return vec3_make(m->m[0][0] * v.x + m->m[0][1] * v.y + m->m[0][2] * v.z,
			 m->m[1][0] * v.x + m->m[1][1] * v.y + m->m[1][2] * v.z,
			 m->m[2][0] * v.x + m->m[2][1] * v.y + m->m[2][2] * v.z);
}

static inline struct mat3 mat3_identity(void)
{
	struct mat3 r = { .m = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };

	return r;
}

static inline struct mat3 mat3_mul(struct mat3 a, struct mat3 b)
{
	struct mat3 out;

	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] +
				      a.m[r][2] * b.m[2][c];
		}
	}
	return out;
}

static inline struct mat3 mat3_rot_x(float rad)
{
	struct mat3 r = mat3_identity();
	const float c = cosf(rad);
	const float s = sinf(rad);

	r.m[1][1] = c;
	r.m[1][2] = -s;
	r.m[2][1] = s;
	r.m[2][2] = c;
	return r;
}

static inline struct mat3 mat3_rot_y(float rad)
{
	struct mat3 r = mat3_identity();
	const float c = cosf(rad);
	const float s = sinf(rad);

	r.m[0][0] = c;
	r.m[0][2] = s;
	r.m[2][0] = -s;
	r.m[2][2] = c;
	return r;
}

static inline struct mat3 mat3_rot_z(float rad)
{
	struct mat3 r = mat3_identity();
	const float c = cosf(rad);
	const float s = sinf(rad);

	r.m[0][0] = c;
	r.m[0][1] = -s;
	r.m[1][0] = s;
	r.m[1][1] = c;
	return r;
}
