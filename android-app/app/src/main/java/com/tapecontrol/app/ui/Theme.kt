package com.tapecontrol.app.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.ExperimentalTextApi
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontVariation
import androidx.compose.ui.text.font.FontWeight

/** Палітра — один в один зі змінних CSS веб-версії. */
object TC {
    val Bg = Color(0xFF0A0E14)
    val Panel = Color(0xFF111820)
    val Border = Color(0xFF1E3A4A)
    val Accent = Color(0xFF00D4FF)
    val Accent2 = Color(0xFFFF6B00)
    val Green = Color(0xFF00FF88)
    val Red = Color(0xFFFF2244)
    val Text = Color(0xFFC8E0E8)
    val Dim = Color(0xFF3A5060)
}

class AppFonts(val display: FontFamily, val mono: FontFamily)

val LocalFonts = staticCompositionLocalOf { AppFonts(FontFamily.SansSerif, FontFamily.Monospace) }

/**
 * Шрифти Orbitron і Share Tech Mono підтягуються з res/font, якщо вони там є
 * (їх завантажує workflow під час збірки). Якщо файлів немає — системні шрифти,
 * додаток від цього не ламається.
 */
@OptIn(ExperimentalTextApi::class)
@Composable
fun TapeTheme(content: @Composable () -> Unit) {
    val ctx = LocalContext.current
    val fonts = remember {
        fun res(name: String) = ctx.resources.getIdentifier(name, "font", ctx.packageName)

        val orbitron = res("orbitron")
        val shareTechMono = res("share_tech_mono")

        val display = if (orbitron != 0) {
            runCatching {
                FontFamily(
                    Font(
                        orbitron, FontWeight.Bold,
                        variationSettings = FontVariation.Settings(FontVariation.weight(700)),
                    ),
                    Font(
                        orbitron, FontWeight.Black,
                        variationSettings = FontVariation.Settings(FontVariation.weight(900)),
                    ),
                )
            }.getOrDefault(FontFamily.SansSerif)
        } else {
            FontFamily.SansSerif
        }

        val mono = if (shareTechMono != 0) {
            runCatching { FontFamily(Font(shareTechMono)) }.getOrDefault(FontFamily.Monospace)
        } else {
            FontFamily.Monospace
        }
        AppFonts(display, mono)
    }
    CompositionLocalProvider(LocalFonts provides fonts, content = content)
}
