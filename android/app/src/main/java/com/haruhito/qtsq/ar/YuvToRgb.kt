package com.haruhito.qtsq.ar

import android.media.Image

/**
 * Nearest-neighbour downscale of an ARCore YUV_420_888 camera image to a small
 * width×height RGB byte buffer (R,G,B per pixel, 0..255) for on-device training.
 *
 * We sample the raw sensor image directly and pair it with
 * Camera.getImageIntrinsics() (same coordinate frame), so pose + projection stay
 * consistent without any display-rotation handling. BT.601 YUV→RGB.
 */
object YuvToRgb {

    fun downscale(image: Image, outW: Int, outH: Int, out: ByteArray) {
        require(out.size >= outW * outH * 3) { "out buffer too small" }
        val srcW = image.width
        val srcH = image.height

        val yPlane = image.planes[0]
        val uPlane = image.planes[1]
        val vPlane = image.planes[2]
        val yBuf = yPlane.buffer
        val uBuf = uPlane.buffer
        val vBuf = vPlane.buffer
        val yRowStride = yPlane.rowStride
        val yPixStride = yPlane.pixelStride
        val uRowStride = uPlane.rowStride
        val uPixStride = uPlane.pixelStride
        val vRowStride = vPlane.rowStride
        val vPixStride = vPlane.pixelStride

        var o = 0
        for (y in 0 until outH) {
            val sy = y * srcH / outH
            val cy = sy / 2
            for (x in 0 until outW) {
                val sx = x * srcW / outW
                val cx = sx / 2

                val yy = (yBuf.get(sy * yRowStride + sx * yPixStride).toInt() and 0xFF)
                val uu = (uBuf.get(cy * uRowStride + cx * uPixStride).toInt() and 0xFF) - 128
                val vv = (vBuf.get(cy * vRowStride + cx * vPixStride).toInt() and 0xFF) - 128

                var r = yy + (1.370705f * vv).toInt()
                var g = yy - (0.337633f * uu).toInt() - (0.698001f * vv).toInt()
                var b = yy + (1.732446f * uu).toInt()
                if (r < 0) r = 0 else if (r > 255) r = 255
                if (g < 0) g = 0 else if (g > 255) g = 255
                if (b < 0) b = 0 else if (b > 255) b = 255

                out[o++] = r.toByte()
                out[o++] = g.toByte()
                out[o++] = b.toByte()
            }
        }
    }
}
