package com.esp32s3.imusim

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.view.View
import kotlin.math.sqrt

sealed class SceneFrame {
    data class RawDerived(
        val distanceM: Float,
        val zoomX: Float,
        val zoomY: Float,
        val zoomZ: Float,
        val footerX: Float,
        val footerY: Float,
        val footerZ: Float,
        val screenW: Int,
        val screenH: Int,
        val rot: FloatArray,
    ) : SceneFrame()

    data class ComputedBoard(
        val distanceM: Float,
        val footerX: Float,
        val footerY: Float,
        val footerZ: Float,
        val zoomX: Float,
        val zoomY: Float,
        val zoomZ: Float,
        val screenW: Int,
        val screenH: Int,
        val rot: FloatArray,
        val axes: FloatArray,
    ) : SceneFrame()

    data class SceneDirect(val record: ImuProtocol.SceneRecord, val screenW: Int, val screenH: Int) : SceneFrame()
}

class ImuSceneView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    init {
        setLayerType(LAYER_TYPE_HARDWARE, null)
    }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    var fpsMeter: FpsMeter? = null
    private var fpsHudLine: String? = null
    private var frame: SceneFrame? = null
    private var powerStatus: ImuProtocol.PowerStatus? = null
    private var clockTzMin: Int? = null
    private var clockSynced = false
    private var scaleX = 1f
    private var scaleY = 1f
    private var offsetX = 0f
    private var offsetY = 0f
    private val headerHandler = Handler(Looper.getMainLooper())
    private val headerTick = object : Runnable {
        override fun run() {
            if (frame != null) {
                invalidate()
                headerHandler.postDelayed(this, 1000L)
            }
        }
    }

    fun setClockSynced(synced: Boolean) {
        if (clockSynced == synced) return
        clockSynced = synced
        invalidate()
    }

    fun setClockTzMin(tzMin: Int?) {
        if (clockTzMin == tzMin) return
        clockTzMin = tzMin
        invalidate()
    }

    fun setFpsHud(line: String?) {
        if (fpsHudLine == line) return
        fpsHudLine = line
        invalidate()
    }

    fun setFrame(f: SceneFrame?) {
        // Always invalidate — SceneFrame is a data class with FloatArray fields, so value
        // equality treats consecutive identical geometry as "unchanged" and skips redraw.
        // That dropped draw FPS to ~1 (header tick only) while BLE/UI meters stayed ~30.
        frame = f
        headerHandler.removeCallbacks(headerTick)
        if (f != null) {
            headerHandler.post(headerTick)
        }
        invalidate()
    }

    fun setPowerStatus(p: ImuProtocol.PowerStatus?) {
        if (powerStatus == p) return
        powerStatus = p
        invalidate()
    }

    override fun onDetachedFromWindow() {
        headerHandler.removeCallbacks(headerTick)
        super.onDetachedFromWindow()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        fpsMeter?.onDraw()
        canvas.drawColor(SceneColors.BG)
        val f = frame ?: run {
            drawFpsHud(canvas)
            return
        }
        updateTransform(f)
        when (f) {
            is SceneFrame.RawDerived -> drawDerived(canvas, f)
            is SceneFrame.ComputedBoard -> drawComputedBoard(canvas, f)
            is SceneFrame.SceneDirect -> drawSceneDirect(canvas, f)
        }
        drawFpsHud(canvas)
    }

    private fun drawFpsHud(canvas: Canvas) {
        val line = fpsHudLine ?: return
        paint.style = Paint.Style.FILL
        paint.color = SceneColors.TEXT
        paint.textSize = (11f * scaleY).coerceAtLeast(9f)
        val fm = paint.fontMetrics
        val x = width - paint.measureText(line) - 8f
        val y = 16f - fm.ascent
        canvas.drawText(line, x, y, paint)
    }

    private fun updateTransform(f: SceneFrame) {
        val sw = when (f) {
            is SceneFrame.RawDerived -> f.screenW
            is SceneFrame.ComputedBoard -> f.screenW
            is SceneFrame.SceneDirect -> f.screenW
        }
        val sh = when (f) {
            is SceneFrame.RawDerived -> f.screenH
            is SceneFrame.ComputedBoard -> f.screenH
            is SceneFrame.SceneDirect -> f.screenH
        }
        scaleX = width.toFloat() / sw
        scaleY = height.toFloat() / sh
        val sx = scaleX.coerceAtMost(scaleY)
        val sy = sx
        scaleX = sx
        scaleY = sy
        offsetX = (width - sw * sx) / 2f
        offsetY = (height - sh * sy) / 2f
    }

    private fun mapX(x: Float) = offsetX + x * scaleX
    private fun mapY(y: Float) = offsetY + y * scaleY

    private fun drawDerived(canvas: Canvas, f: SceneFrame.RawDerived) {
        val sw = f.screenW
        val sh = f.screenH
        val cam = Camera3D(aspect = sw.toFloat() / sh.toFloat())
        val cx = sw / 2
        val cy = sh / 2 + 10

        paint.style = Paint.Style.FILL
        paint.color = SceneColors.TEXT
        canvas.drawCircle(mapX(cx.toFloat()), mapY(cy.toFloat()), 3f * scaleX, paint)

        drawAxis(canvas, f.rot, f.zoomX, f.zoomY, f.zoomZ, cam, sw, sh, Vec3(1f, 0f, 0f), 1.2f, SceneColors.AXIS_X)
        drawAxis(canvas, f.rot, f.zoomX, f.zoomY, f.zoomZ, cam, sw, sh, Vec3(0f, 1f, 0f), 1.2f, SceneColors.AXIS_Y)
        drawAxis(canvas, f.rot, f.zoomX, f.zoomY, f.zoomZ, cam, sw, sh, Vec3(0f, 0f, 1f), 1.2f, SceneColors.AXIS_Z)

        drawCube(canvas, f.rot, f.zoomX, f.zoomY, f.zoomZ, cam, sw, sh)
        drawHeaderBar(canvas, f.distanceM)
        drawFooter(canvas, sw, sh, f.footerX, f.footerY, f.footerZ)
        drawAxisLabels(canvas, sh)
    }

    private fun drawComputedBoard(canvas: Canvas, f: SceneFrame.ComputedBoard) {
        val sw = f.screenW
        val sh = f.screenH
        val cam = Camera3D(aspect = sw.toFloat() / sh.toFloat())
        val cx = sw / 2
        val cy = sh / 2 + 10

        paint.style = Paint.Style.FILL
        paint.color = SceneColors.TEXT
        canvas.drawCircle(mapX(cx.toFloat()), mapY(cy.toFloat()), 3f * scaleX, paint)

        paint.strokeWidth = 2f * scaleX
        paint.style = Paint.Style.STROKE
        if (hasBoardAxes(f.axes)) {
            val axisColors = intArrayOf(SceneColors.AXIS_X, SceneColors.AXIS_Y, SceneColors.AXIS_Z)
            for (i in 0 until 3) {
                paint.color = axisColors[i]
                val base = i * 4
                canvas.drawLine(
                    mapX(f.axes[base]),
                    mapY(f.axes[base + 1]),
                    mapX(f.axes[base + 2]),
                    mapY(f.axes[base + 3]),
                    paint,
                )
            }
        } else {
            drawAxis(canvas, f.rot, f.zoomX, f.zoomY, f.zoomZ, cam, sw, sh, Vec3(1f, 0f, 0f), 1.2f, SceneColors.AXIS_X)
            drawAxis(canvas, f.rot, f.zoomX, f.zoomY, f.zoomZ, cam, sw, sh, Vec3(0f, 1f, 0f), 1.2f, SceneColors.AXIS_Y)
            drawAxis(canvas, f.rot, f.zoomX, f.zoomY, f.zoomZ, cam, sw, sh, Vec3(0f, 0f, 1f), 1.2f, SceneColors.AXIS_Z)
        }

        drawCube(canvas, f.rot, f.zoomX, f.zoomY, f.zoomZ, cam, sw, sh)
        drawHeaderBar(canvas, f.distanceM)
        drawFooter(canvas, sw, sh, f.footerX, f.footerY, f.footerZ)
        drawAxisLabels(canvas, sh)
    }

    private fun drawSceneDirect(canvas: Canvas, f: SceneFrame.SceneDirect) {
        val r = f.record
        val sh = f.screenH
        val cx = f.screenW / 2
        val cy = f.screenH / 2 + 10

        paint.style = Paint.Style.FILL
        paint.color = SceneColors.TEXT
        canvas.drawCircle(mapX(cx.toFloat()), mapY(cy.toFloat()), 3f * scaleX, paint)

        paint.strokeWidth = 2f * scaleX
        paint.style = Paint.Style.STROKE
        val axisColors = intArrayOf(SceneColors.AXIS_X, SceneColors.AXIS_Y, SceneColors.AXIS_Z)
        for (i in 0 until 3) {
            paint.color = axisColors[i]
            val base = i * 4
            canvas.drawLine(
                mapX(r.axes[base]),
                mapY(r.axes[base + 1]),
                mapX(r.axes[base + 2]),
                mapY(r.axes[base + 3]),
                paint,
            )
        }

        paint.color = SceneColors.TEXT
        for (edge in SceneColors.cubeEdges()) {
            val a = edge[0]
            val b = edge[1]
            canvas.drawLine(
                mapX(r.corners[a * 2]),
                mapY(r.corners[a * 2 + 1]),
                mapX(r.corners[b * 2]),
                mapY(r.corners[b * 2 + 1]),
                paint,
            )
        }

        drawHeaderBar(canvas, r.distanceM)
        drawFooter(canvas, f.screenW, sh, r.footerX, r.footerY, r.footerZ)
        drawAxisLabels(canvas, sh)
    }

    private fun drawAxis(
        canvas: Canvas,
        rot: FloatArray,
        zoomX: Float,
        zoomY: Float,
        zoomZ: Float,
        cam: Camera3D,
        sw: Int,
        sh: Int,
        dir: Vec3,
        scale: Float,
        color: Int,
    ) {
        val end = Projection.transform(rot, Projection.applyZoom(zoomX, zoomY, zoomZ, normalize(dir * scale)))
        val p0 = Projection.project(Vec3(0f, 0f, 2.5f), sw, sh, cam)
        val p1 = Projection.project(Vec3(end.x, end.y, 2.5f + end.z), sw, sh, cam)
        paint.color = color
        paint.strokeWidth = 2f * scaleX
        paint.style = Paint.Style.STROKE
        canvas.drawLine(mapX(p0.x), mapY(p0.y), mapX(p1.x), mapY(p1.y), paint)
    }

    private fun drawCube(canvas: Canvas, rot: FloatArray, zoomX: Float, zoomY: Float, zoomZ: Float, cam: Camera3D, sw: Int, sh: Int) {
        val unit = arrayOf(
            Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, -0.5f), Vec3(-0.5f, 0.5f, -0.5f),
            Vec3(-0.5f, -0.5f, 0.5f), Vec3(0.5f, -0.5f, 0.5f), Vec3(0.5f, 0.5f, 0.5f), Vec3(-0.5f, 0.5f, 0.5f),
        )
        val projected = Array(8) { i ->
            var p = Projection.transform(rot, Projection.applyZoom(zoomX, zoomY, zoomZ, unit[i]))
            p = Vec3(p.x, p.y, p.z + 2f)
            Projection.project(p, sw, sh, cam)
        }
        paint.color = SceneColors.TEXT
        paint.strokeWidth = 2f * scaleX
        paint.style = Paint.Style.STROKE
        for (edge in SceneColors.cubeEdges()) {
            val a = edge[0]
            val b = edge[1]
            canvas.drawLine(
                mapX(projected[a].x),
                mapY(projected[a].y),
                mapX(projected[b].x),
                mapY(projected[b].y),
                paint,
            )
        }
    }

    private fun drawHeaderBar(canvas: Canvas, distanceM: Float) {
        val hudX = 10f
        val hudY = 20f
        val hudW = 168f
        val hudH = 16f
        paint.style = Paint.Style.FILL
        paint.color = SceneColors.BG
        canvas.drawRect(mapX(hudX), mapY(hudY), mapX(hudX + hudW), mapY(hudY + hudH), paint)

        paint.color = SceneColors.WALK
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1.5f * scaleX
        val tx = mapX(hudX + 2f)
        val ty = mapY(hudY + 2f)
        canvas.drawLine(tx, ty + 9f * scaleY, tx + 4f * scaleX, ty, paint)
        canvas.drawLine(tx + 4f * scaleX, ty, tx + 8f * scaleX, ty + 9f * scaleY, paint)

        paint.style = Paint.Style.FILL
        paint.textSize = (12f * scaleY).coerceAtLeast(10f)
        val headerLabel = HeaderRotator.label(distanceM, powerStatus, tzMin = clockTzMin, clockSynced = clockSynced)
        val fm = paint.fontMetrics
        val textX = mapX(hudX + 14f)
        val textY = mapY(hudY + 3f) - fm.ascent
        canvas.drawText(headerLabel, textX, textY, paint)
    }

    private fun drawFooter(canvas: Canvas, sw: Int, sh: Int, fx: Float, fy: Float, fz: Float) {
        val footerY = sh - 28
        paint.style = Paint.Style.FILL
        paint.color = SceneColors.BG
        canvas.drawRect(mapX(0f), mapY(footerY - 2f), mapX(sw.toFloat()), mapY(footerY + 12f), paint)
        paint.color = SceneColors.AXIS_Y
        paint.textSize = 12f * scaleY
        canvas.drawText(
            String.format("2D->3D: %.2f %.2f %.2f", fx, fy, fz),
            mapX(4f),
            mapY(footerY + 10f),
            paint,
        )
    }

    private fun drawAxisLabels(canvas: Canvas, sh: Int) {
        val y = sh - 40
        paint.textSize = 14f * scaleY
        paint.color = SceneColors.AXIS_X
        canvas.drawText("X", mapX(4f), mapY(y + 14f), paint)
        paint.color = SceneColors.AXIS_Y
        canvas.drawText("Y", mapX(24f), mapY(y + 14f), paint)
        paint.color = SceneColors.AXIS_Z
        canvas.drawText("Z", mapX(44f), mapY(y + 14f), paint)
    }

    private fun hasBoardAxes(axes: FloatArray): Boolean {
        if (axes.size < 12) return false
        for (v in axes) {
            if (v != 0f) return true
        }
        return false
    }

    private fun normalize(v: Vec3): Vec3 {
        val len = sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
        if (len < 1e-6f) return Vec3(0f, 0f, 0f)
        return Vec3(v.x / len, v.y / len, v.z / len)
    }

    private operator fun Vec3.times(s: Float) = Vec3(x * s, y * s, z * s)
}
