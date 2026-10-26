package com.emux.translator.util

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import java.io.File
import java.io.FileOutputStream

object ImageUpscaler {

    /**
     * Esegue un upscaling rapido (bicubico HQ + filtro unsharp mask leggero) portando l'immagine a targetSize x targetSize.
     * Salva il risultato su disk nel file di output e lo restituisce.
     * Non sovrascrive se outFile esiste già.
     */
    fun getOrUpscaleImage(sourceFile: File, outputFile: File, targetSize: Int = 512): File {
        if (outputFile.exists() && outputFile.length() > 0) {
            return outputFile
        }

        try {
            val original = BitmapFactory.decodeFile(sourceFile.absolutePath) ?: return sourceFile

            val upscaled = upscaleBitmap(original, targetSize)
            
            outputFile.parentFile?.let {
                if (!it.exists()) it.mkdirs()
            }

            FileOutputStream(outputFile).use { out ->
                upscaled.compress(Bitmap.CompressFormat.PNG, 95, out)
            }

            if (upscaled != original) {
                upscaled.recycle()
            }
            original.recycle()

            return outputFile
        } catch (e: Exception) {
            e.printStackTrace()
            return sourceFile
        }
    }

    /**
     * Esegue l'upscale di un Bitmap in memoria con filtro ad alta fedeltà e sharpening adattivo.
     */
    fun upscaleBitmap(source: Bitmap, targetSize: Int = 512): Bitmap {
        if (source.width >= targetSize && source.height >= targetSize) {
            return source
        }

        // 1. Scalatura bilineare/bicubica anti-aliasata con filtro Paint ad alta qualità
        val scaled = Bitmap.createBitmap(targetSize, targetSize, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(scaled)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG or Paint.DITHER_FLAG)

        // Calcola aspect ratio centrato (crop/fill quadrato 1:1)
        val srcW = source.width
        val srcH = source.height
        val scale = maxOf(targetSize.toFloat() / srcW, targetSize.toFloat() / srcH)
        val destW = (srcW * scale).toInt()
        val destH = (srcH * scale).toInt()
        val left = (targetSize - destW) / 2
        val top = (targetSize - destH) / 2

        val destRect = android.graphics.Rect(left, top, left + destW, top + destH)
        val srcRect = android.graphics.Rect(0, 0, srcW, srcH)
        canvas.drawBitmap(source, srcRect, destRect, paint)

        // 2. Applicazione veloce di unsharp enhancement per bordi nitidi sui dettagli delle copertine
        val sharpened = applySharpen(scaled)
        if (sharpened != scaled) {
            scaled.recycle()
            return sharpened
        }

        return scaled
    }

    private fun applySharpen(src: Bitmap): Bitmap {
        val width = src.width
        val height = src.height
        val output = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)

        val pixels = IntArray(width * height)
        val outPixels = IntArray(width * height)
        src.getPixels(pixels, 0, width, 0, 0, width, height)

        // Maschera di nitidezza (3x3 Laplacian / Unsharp Kernel):
        // [  0, -1,  0 ]
        // [ -1,  5, -1 ]
        // [  0, -1,  0 ]
        for (y in 1 until height - 1) {
            val yOffset = y * width
            for (x in 1 until width - 1) {
                val idx = yOffset + x

                val cCenter = pixels[idx]
                val cTop = pixels[idx - width]
                val cBottom = pixels[idx + width]
                val cLeft = pixels[idx - 1]
                val cRight = pixels[idx + 1]

                val a = (cCenter shr 24) and 0xff
                var r = (5 * ((cCenter shr 16) and 0xff)) - ((cTop shr 16) and 0xff) - ((cBottom shr 16) and 0xff) - ((cLeft shr 16) and 0xff) - ((cRight shr 16) and 0xff)
                var g = (5 * ((cCenter shr 8) and 0xff)) - ((cTop shr 8) and 0xff) - ((cBottom shr 8) and 0xff) - ((cLeft shr 8) and 0xff) - ((cRight shr 8) and 0xff)
                var b = (5 * (cCenter and 0xff)) - (cTop and 0xff) - (cBottom and 0xff) - (cLeft and 0xff) - (cRight and 0xff)

                r = r.coerceIn(0, 255)
                g = g.coerceIn(0, 255)
                b = b.coerceIn(0, 255)

                outPixels[idx] = (a shl 24) or (r shl 16) or (g shl 8) or b
            }
        }

        // Bordo superiore e inferiore copia 1:1
        System.arraycopy(pixels, 0, outPixels, 0, width)
        System.arraycopy(pixels, (height - 1) * width, outPixels, (height - 1) * width, width)

        output.setPixels(outPixels, 0, width, 0, 0, width, height)
        return output
    }
}
