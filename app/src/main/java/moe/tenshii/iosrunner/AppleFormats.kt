package moe.tenshii.iosrunner

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Xml
import org.xmlpull.v1.XmlPullParser
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.util.zip.Inflater

/** Parses binary and XML property lists into Map, List, String, Long, Double, Boolean and ByteArray values. */
object PropertyList {
    fun parse(bytes: ByteArray): Any? =
        if (bytes.size >= 8 && String(bytes, 0, 6, Charsets.US_ASCII) == "bplist") parseBinary(bytes)
        else parseXml(bytes)

    private fun parseBinary(data: ByteArray): Any? {
        require(data.size >= 40) { "truncated property list" }
        val trailer = ByteBuffer.wrap(data, data.size - 32, 32)
        trailer.position(trailer.position() + 6)
        val offsetSize = trailer.get().toInt()
        val refSize = trailer.get().toInt()
        val objectCount = trailer.long.toInt()
        val topObject = trailer.long.toInt()
        val tableOffset = trailer.long.toInt()

        fun readSized(position: Int, size: Int): Long {
            var value = 0L
            for (i in 0 until size) value = (value shl 8) or (data[position + i].toLong() and 0xff)
            return value
        }

        fun offsetOf(index: Int): Int {
            require(index in 0 until objectCount) { "object reference out of range" }
            return readSized(tableOffset + index * offsetSize, offsetSize).toInt()
        }

        fun read(index: Int, depth: Int): Any? {
            require(depth < 64) { "property list nested too deeply" }
            val start = offsetOf(index)
            val marker = data[start].toInt() and 0xff
            val high = marker shr 4
            var count = marker and 0x0f
            var body = start + 1
            if (high != 0 && high != 1 && high != 2 && high != 3 && count == 0x0f) {
                val intSize = 1 shl (data[body].toInt() and 0x0f)
                count = readSized(body + 1, intSize).toInt()
                body += 1 + intSize
            }
            return when (high) {
                0x0 -> when (marker) {
                    0x08 -> false
                    0x09 -> true
                    else -> null
                }
                0x1 -> readSized(body, 1 shl count)
                0x2 -> if (count == 2) ByteBuffer.wrap(data, body, 4).float.toDouble() else ByteBuffer.wrap(data, body, 8).double
                0x3 -> ByteBuffer.wrap(data, body, 8).double
                0x4 -> data.copyOfRange(body, body + count)
                0x5 -> String(data, body, count, Charsets.US_ASCII)
                0x6 -> String(data, body, count * 2, Charsets.UTF_16BE)
                0x8 -> readSized(body, count + 1)
                0xA, 0xC -> List(count) { read(readSized(body + it * refSize, refSize).toInt(), depth + 1) }
                0xD -> buildMap {
                    for (i in 0 until count) {
                        val key = read(readSized(body + i * refSize, refSize).toInt(), depth + 1) as? String ?: continue
                        put(key, read(readSized(body + (count + i) * refSize, refSize).toInt(), depth + 1))
                    }
                }
                else -> null
            }
        }
        return read(topObject, 0)
    }

    private fun parseXml(bytes: ByteArray): Any? {
        val parser = Xml.newPullParser()
        parser.setInput(ByteArrayInputStream(bytes), null)
        while (parser.next() != XmlPullParser.END_DOCUMENT) {
            if (parser.eventType == XmlPullParser.START_TAG && parser.name != "plist") return readXmlValue(parser)
        }
        return null
    }

    private fun readXmlValue(parser: XmlPullParser): Any? = when (parser.name) {
        "dict" -> buildMap {
            while (parser.nextTag() == XmlPullParser.START_TAG) {
                val key = parser.nextText()
                parser.nextTag()
                put(key, readXmlValue(parser))
            }
        }
        "array" -> buildList {
            while (parser.nextTag() == XmlPullParser.START_TAG) add(readXmlValue(parser))
        }
        "string" -> parser.nextText()
        "integer" -> parser.nextText().trim().toLongOrNull()
        "real" -> parser.nextText().trim().toDoubleOrNull()
        "true" -> { parser.nextTag(); true }
        "false" -> { parser.nextTag(); false }
        else -> { parser.nextText(); null }
    }
}

/** Decodes app icons, including the CgBI PNG variant Xcode produces, which BitmapFactory rejects. */
object AppleIcon {
    private const val SIGNATURE_LENGTH = 8

    fun decode(bytes: ByteArray): Bitmap? {
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.let { return it }
        return runCatching { decodeCgbi(bytes) }.getOrNull()
    }

    private fun decodeCgbi(bytes: ByteArray): Bitmap? {
        val buffer = ByteBuffer.wrap(bytes)
        buffer.position(SIGNATURE_LENGTH)
        var width = 0
        var height = 0
        var isCgbi = false
        val compressed = ByteArrayOutputStream()
        while (buffer.remaining() >= 12) {
            val length = buffer.int
            val type = ByteArray(4).also { buffer.get(it) }.toString(Charsets.US_ASCII)
            val chunkStart = buffer.position()
            when (type) {
                "CgBI" -> isCgbi = true
                "IHDR" -> {
                    width = buffer.int
                    height = buffer.int
                    val bitDepth = buffer.get().toInt()
                    val colorType = buffer.get().toInt()
                    buffer.position(buffer.position() + 2)
                    val interlace = buffer.get().toInt()
                    if (bitDepth != 8 || colorType != 6 || interlace != 0) return null
                }
                "IDAT" -> compressed.write(bytes, chunkStart, length)
            }
            if (type == "IEND") break
            buffer.position(chunkStart + length + 4)
        }
        if (!isCgbi || width <= 0 || height <= 0) return null

        val stride = width * 4
        val raw = ByteArray((stride + 1) * height)
        val inflater = Inflater(true)
        inflater.setInput(compressed.toByteArray())
        var written = 0
        while (written < raw.size && !inflater.finished()) {
            val count = inflater.inflate(raw, written, raw.size - written)
            if (count == 0) break
            written += count
        }
        inflater.end()
        if (written != raw.size) return null

        val pixels = IntArray(width * height)
        val previous = ByteArray(stride)
        val current = ByteArray(stride)
        for (y in 0 until height) {
            val rowStart = y * (stride + 1)
            val filter = raw[rowStart].toInt()
            for (x in 0 until stride) {
                val value = raw[rowStart + 1 + x].toInt() and 0xff
                val left = if (x >= 4) current[x - 4].toInt() and 0xff else 0
                val up = previous[x].toInt() and 0xff
                val upLeft = if (x >= 4) previous[x - 4].toInt() and 0xff else 0
                val predicted = when (filter) {
                    1 -> left
                    2 -> up
                    3 -> (left + up) / 2
                    4 -> paeth(left, up, upLeft)
                    else -> 0
                }
                current[x] = (value + predicted).toByte()
            }
            for (x in 0 until width) {
                val blue = current[x * 4].toInt() and 0xff
                val green = current[x * 4 + 1].toInt() and 0xff
                val red = current[x * 4 + 2].toInt() and 0xff
                val alpha = current[x * 4 + 3].toInt() and 0xff
                pixels[y * width + x] = (alpha shl 24) or (red shl 16) or (green shl 8) or blue
            }
            current.copyInto(previous)
        }
        return Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
    }

    private fun paeth(a: Int, b: Int, c: Int): Int {
        val p = a + b - c
        val pa = kotlin.math.abs(p - a)
        val pb = kotlin.math.abs(p - b)
        val pc = kotlin.math.abs(p - c)
        return if (pa <= pb && pa <= pc) a else if (pb <= pc) b else c
    }
}
