package com.haruhito.qtsq.ar

import android.opengl.GLES20
import com.google.ar.core.PointCloud

/**
 * Draws ARCore feature points as GL points. This is the live "geometry is being
 * captured" preview for Phase C — cheap, and it confirms the tracker is finding
 * structure to reconstruct. (The surfel renderer replaces this in Phase D.)
 */
class PointCloudRenderer {

    private var program = 0
    private var aPosition = 0
    private var uMvp = 0
    private var uColor = 0
    private var uPointSize = 0
    private var vbo = 0
    private var vboCapacityFloats = 0

    private val vertexShader = """
        uniform mat4 u_MVP;
        uniform float u_PointSize;
        attribute vec4 a_Position;
        void main() {
            gl_Position = u_MVP * vec4(a_Position.xyz, 1.0);
            gl_PointSize = u_PointSize;
        }
    """.trimIndent()

    private val fragmentShader = """
        precision mediump float;
        uniform vec4 u_Color;
        void main() { gl_FragColor = u_Color; }
    """.trimIndent()

    fun createOnGlThread() {
        val buffers = IntArray(1)
        GLES20.glGenBuffers(1, buffers, 0)
        vbo = buffers[0]
        vboCapacityFloats = 4 * 1000
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo)
        GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, vboCapacityFloats * 4, null, GLES20.GL_DYNAMIC_DRAW)
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0)

        program = GlUtil.buildProgram(vertexShader, fragmentShader)
        aPosition = GLES20.glGetAttribLocation(program, "a_Position")
        uMvp = GLES20.glGetUniformLocation(program, "u_MVP")
        uColor = GLES20.glGetUniformLocation(program, "u_Color")
        uPointSize = GLES20.glGetUniformLocation(program, "u_PointSize")
    }

    /** mvp = projection * view (column-major 4x4). */
    fun draw(pointCloud: PointCloud, mvp: FloatArray) {
        val points = pointCloud.points        // FloatBuffer, 4 floats (x,y,z,confidence) per point
        val numPoints = points.remaining() / 4
        if (numPoints == 0) return

        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo)
        val neededFloats = numPoints * 4
        if (neededFloats > vboCapacityFloats) {
            vboCapacityFloats = neededFloats * 2
            GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, vboCapacityFloats * 4, null, GLES20.GL_DYNAMIC_DRAW)
        }
        points.position(0)
        GLES20.glBufferSubData(GLES20.GL_ARRAY_BUFFER, 0, neededFloats * 4, points)

        GLES20.glUseProgram(program)
        GLES20.glEnableVertexAttribArray(aPosition)
        GLES20.glVertexAttribPointer(aPosition, 4, GLES20.GL_FLOAT, false, 16, 0)
        GLES20.glUniformMatrix4fv(uMvp, 1, false, mvp, 0)
        GLES20.glUniform4f(uColor, 0.2f, 0.9f, 0.5f, 1.0f)   // QTSQ green
        GLES20.glUniform1f(uPointSize, 6.0f)

        GLES20.glDrawArrays(GLES20.GL_POINTS, 0, numPoints)

        GLES20.glDisableVertexAttribArray(aPosition)
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0)
    }
}
