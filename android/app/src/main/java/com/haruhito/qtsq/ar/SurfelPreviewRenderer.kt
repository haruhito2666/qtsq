package com.haruhito.qtsq.ar

import android.opengl.GLES20
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * Phase D §5 — live surfel preview (GLES).
 *
 * Draws the trainer's current surfel snapshot (float[n*13]:
 * center[3], quat[4], log_su, log_sv, opacity_logit, color[3]) as
 * camera-facing billboarded quads in world space. The actual view
 * orientation comes from the ARCore camera (uv axes of the billboard are
 * taken from the view matrix rows), so splats rotate correctly with the
 * device. Size comes straight from exp(log_scale) metres; blending uses
 * sigmoid(opacity_logit) like the renderer.
 *
 * Replaces PointCloudRenderer's role once training starts: the user sees
 * the model forming in real time instead of raw feature points.
 */
class SurfelPreviewRenderer {

    private var program = 0
    private var aCenter = 0
    private var aOffset = 0
    private var aColor = 0
    private var aAlpha = 0
    private var uMvp = 0
    private var uCamRight = 0
    private var uCamUp = 0
    private var vertexBuffer: FloatBuffer? = null
    private var vbo = 0
    private var capacityVerts = 0

    fun createOnGlThread() {
        val vs = """#version 100
            attribute vec3 aCenter;
            attribute vec2 aOffset;
            attribute vec3 aColor;
            attribute float aAlpha;
            uniform mat4 uMvp;
            uniform vec3 uCamRight;   // camera right axis in world space
            uniform vec3 uCamUp;      // camera up axis in world space
            varying vec3 vColor;
            varying float vAlpha;
            void main() {
                vec3 world = aCenter + uCamRight * aOffset.x + uCamUp * aOffset.y;
                gl_Position = uMvp * vec4(world, 1.0);
                vColor = aColor;
                vAlpha = aAlpha;
            }
        """

        val fs = """#version 100
            precision mediump float;
            varying vec3 vColor;
            varying float vAlpha;
            void main() {
                gl_FragColor = vec4(vColor * vAlpha, vAlpha);
            }
        """

        program = GLES20.glCreateProgram()
        val shaders = intArrayOf(
            compileShader(GLES20.GL_VERTEX_SHADER, vs),
            compileShader(GLES20.GL_FRAGMENT_SHADER, fs)
        )
        GLES20.glAttachShader(program, shaders[0])
        GLES20.glAttachShader(program, shaders[1])
        GLES20.glLinkProgram(program)
        GLES20.glDetachShader(program, shaders[0])
        GLES20.glDetachShader(program, shaders[1])
        GLES20.glDeleteShader(shaders[0])
        GLES20.glDeleteShader(shaders[1])

        aCenter = GLES20.glGetAttribLocation(program, "aCenter")
        aOffset = GLES20.glGetAttribLocation(program, "aOffset")
        aColor = GLES20.glGetAttribLocation(program, "aColor")
        aAlpha = GLES20.glGetAttribLocation(program, "aAlpha")
        uMvp = GLES20.glGetUniformLocation(program, "uMvp")
        uCamRight = GLES20.glGetUniformLocation(program, "uCamRight")
        uCamUp = GLES20.glGetUniformLocation(program, "uCamUp")

        val ids = IntArray(1)
        GLES20.glGenBuffers(1, ids, 0)
        vbo = ids[0]
    }

    private fun compileShader(type: Int, src: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, src)
        GLES20.glCompileShader(shader)
        return shader
    }

    /**
     * @param surfels float[n*13] snapshot from QtsqEngine.captureGetSurfels
     * @param mvp Combined proj*view (ARCore camera), world → clip
     * @param view float[16] column-major world→view matrix (for billboard axes)
     */
    fun draw(surfels: FloatArray, mvp: FloatArray, view: FloatArray) {
        if (vbo == 0) return
        val n = surfels.size / 13
        if (n == 0) return

        // Camera axes in world space = rows of the (rigid) view matrix.
        val camRight = floatArrayOf(view[0], view[1], view[2])
        val camUp = floatArrayOf(view[4], view[5], view[6])

        // 6 vertices per surfel: center + (±right + ±up) * half-size.
        // Layout per vertex: center(3) + offset(2) + color(3) + alpha(1) = 9 floats.
        val vertsNeeded = n * 6
        if (vertsNeeded > capacityVerts) {
            capacityVerts = vertsNeeded
            vertexBuffer = ByteBuffer.allocateDirect(vertsNeeded * 9 * 4)
                .order(ByteOrder.nativeOrder()).asFloatBuffer()
        }
        val buf = vertexBuffer ?: return
        buf.clear()

        var vi = 0
        var i = 0
        while (i < n) {
            val b = i * 13
            val cx = surfels[b]
            val cy = surfels[b + 1]
            val cz = surfels[b + 2]
            // (quat unused for a camera-facing billboard v1)
            val halfU = Math.exp(surfels[b + 7].toDouble()).toFloat() * 0.5f
            val halfV = Math.exp(surfels[b + 8].toDouble()).toFloat() * 0.5f
            val alpha = sigmoid(surfels[b + 9])
            val cr = surfels[b + 10].coerceIn(0f, 1f)
            val cg = surfels[b + 11].coerceIn(0f, 1f)
            val cb = surfels[b + 12].coerceIn(0f, 1f)

            // Corners of the billboard in (right, up) offset space.
            val ox = floatArrayOf(-halfU, halfU, halfU, -halfU, -halfU, halfU)
            val oy = floatArrayOf(-halfV, -halfV, halfV, halfV, halfV, -halfV)
            for (v in 0 until 6) {
                buf.put(cx); buf.put(cy); buf.put(cz)
                buf.put(ox[v]); buf.put(oy[v])
                buf.put(cr); buf.put(cg); buf.put(cb)
                buf.put(alpha)
            }
            vi += 6
            i++
        }
        buf.flip()

        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo)
        GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, buf.limit() * 4, buf, GLES20.GL_DYNAMIC_DRAW)

        GLES20.glUseProgram(program)
        GLES20.glUniformMatrix4fv(uMvp, 1, false, mvp, 0)
        GLES20.glUniform3f(uCamRight, camRight[0], camRight[1], camRight[2])
        GLES20.glUniform3f(uCamUp, camUp[0], camUp[1], camUp[2])

        val stride = 9 * 4
        GLES20.glEnableVertexAttribArray(aCenter)
        GLES20.glVertexAttribPointer(aCenter, 3, GLES20.GL_FLOAT, false, stride, 0)
        GLES20.glEnableVertexAttribArray(aOffset)
        GLES20.glVertexAttribPointer(aOffset, 2, GLES20.GL_FLOAT, false, stride, 3 * 4)
        GLES20.glEnableVertexAttribArray(aColor)
        GLES20.glVertexAttribPointer(aColor, 3, GLES20.GL_FLOAT, false, stride, 5 * 4)
        GLES20.glEnableVertexAttribArray(aAlpha)
        GLES20.glVertexAttribPointer(aAlpha, 1, GLES20.GL_FLOAT, false, stride, 8 * 4)

        GLES20.glEnable(GLES20.GL_BLEND)
        GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA, GLES20.GL_ONE_MINUS_SRC_ALPHA)
        GLES20.glEnable(GLES20.GL_DEPTH_TEST)
        GLES20.glDepthFunc(GLES20.GL_LEQUAL)
        GLES20.glDepthMask(true)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, vertsNeeded)

        GLES20.glDisable(GLES20.GL_DEPTH_TEST)
        GLES20.glDisable(GLES20.GL_BLEND)
        GLES20.glDisableVertexAttribArray(aCenter)
        GLES20.glDisableVertexAttribArray(aOffset)
        GLES20.glDisableVertexAttribArray(aColor)
        GLES20.glDisableVertexAttribArray(aAlpha)
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0)
        GLES20.glUseProgram(0)
    }

    private fun sigmoid(x: Float): Float = (1f / (1f + Math.exp((-x).toDouble()))).toFloat()
}
