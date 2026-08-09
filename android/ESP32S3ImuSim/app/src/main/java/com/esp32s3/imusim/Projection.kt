package com.esp32s3.imusim

import kotlin.math.tan

data class Vec2(val x: Float, val y: Float)
data class Vec3(val x: Float, val y: Float, val z: Float)

data class Camera3D(
    val fovDeg: Float = 45f,
    val nearPlane: Float = 0.1f,
    val aspect: Float = 1f,
)

object Projection {
    fun project(point: Vec3, screenW: Int, screenH: Int, cam: Camera3D): Vec2 {
        var z = point.z
        if (z < cam.nearPlane) z = cam.nearPlane
        val f = 1f / tan(Math.toRadians((cam.fovDeg / 2f).toDouble())).toFloat()
        val sx = (point.x * f / (z * cam.aspect)) * (screenW / 2f) + (screenW / 2f)
        val sy = (-point.y * f / z) * (screenH / 2f) + (screenH / 2f)
        return Vec2(sx, sy)
    }

    fun unproject(screen: Vec2, depth: Float, screenW: Int, screenH: Int, cam: Camera3D): Vec3 {
        val f = 1f / tan(Math.toRadians((cam.fovDeg / 2f).toDouble())).toFloat()
        val nx = (screen.x - screenW / 2f) / (screenW / 2f)
        val ny = -(screen.y - screenH / 2f) / (screenH / 2f)
        val x = nx * depth * cam.aspect / f
        val y = ny * depth / f
        return Vec3(x, y, depth)
    }

    fun transform(m: FloatArray, v: Vec3): Vec3 {
        return Vec3(
            m[0] * v.x + m[1] * v.y + m[2] * v.z,
            m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z,
        )
    }

    fun applyZoom(zoom: Float, v: Vec3): Vec3 = Vec3(v.x * zoom, v.y * zoom, v.z * zoom)

    fun applyZoom(zx: Float, zy: Float, zz: Float, v: Vec3): Vec3 = Vec3(v.x * zx, v.y * zy, v.z * zz)
}
