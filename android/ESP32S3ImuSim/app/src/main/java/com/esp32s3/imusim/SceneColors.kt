package com.esp32s3.imusim

import android.graphics.Color
import kotlin.math.roundToInt

object SceneColors {
    // RGB565 palette from esp32 scene_renderer.cpp
    fun rgb565(c: Int): Int {
        val r = ((c shr 11) and 0x1F) * 255 / 31
        val g = ((c shr 5) and 0x3F) * 255 / 63
        val b = (c and 0x1F) * 255 / 31
        return Color.rgb(r, g, b)
    }

    val BG = rgb565(0x0000)
    val TEXT = rgb565(0xFFFF)
    val AXIS_X = rgb565(0xF800)
    val AXIS_Y = rgb565(0x07E0)
    val AXIS_Z = rgb565(0x001F)
    val WALK = rgb565(0xFFE0)
    val WARN = rgb565(0xFD20)

    private val CUBE_EDGES = arrayOf(
        intArrayOf(0, 1), intArrayOf(1, 2), intArrayOf(2, 3), intArrayOf(3, 0),
        intArrayOf(4, 5), intArrayOf(5, 6), intArrayOf(6, 7), intArrayOf(7, 4),
        intArrayOf(0, 4), intArrayOf(1, 5), intArrayOf(2, 6), intArrayOf(3, 7),
    )

    fun cubeEdges(): Array<IntArray> = CUBE_EDGES
}
