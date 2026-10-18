package com.tapecontrol.app.ui

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.tapecontrol.app.LogEntry
import com.tapecontrol.app.ble.LinkState
import com.tapecontrol.app.ble.LogType
import com.tapecontrol.app.ble.TapeStatus

// ---------------------------------------------------------------------------------------------
// Іконки (ті самі SVG-контури 24×24, що й у веб-версії)
// ---------------------------------------------------------------------------------------------

private object Glyphs {
    val rew: Path = PathParser().parsePathString("M11 18V6l-8.5 6 8.5 6zm.5-6l8.5 6V6l-8.5 6z").toPath()
    val fwd: Path = PathParser().parsePathString("M4 18l8.5-6L4 6v12zm9-12v12l8.5-6L13 6z").toPath()
    val play: Path = PathParser().parsePathString("M8 5v14l11-7z").toPath()
    val stop: Path = Path().apply { addRect(Rect(4f, 4f, 20f, 20f)) }
    val rec: Path = Path().apply { addOval(Rect(center = Offset(12f, 12f), radius = 8f)) }
}

private fun rgba(r: Int, g: Int, b: Int, a: Float): Color = Color(r, g, b).copy(alpha = a)

// ---------------------------------------------------------------------------------------------
// Допоміжне
// ---------------------------------------------------------------------------------------------

@Composable
private fun mono(sizeSp: Int, spacingSp: Float, color: Color): TextStyle =
    TextStyle(
        fontFamily = LocalFonts.current.mono,
        fontSize = sizeSp.sp,
        letterSpacing = spacingSp.sp,
        color = color,
    )

private fun Modifier.panel(): Modifier =
    this.background(TC.Panel).border(1.dp, TC.Border)

/** Тонка сітка на фоні (як body::before у веб-версії). */
private fun DrawScope.drawGrid() {
    val step = 40.dp.toPx()
    val c = TC.Accent.copy(alpha = 0.03f)
    var x = 0f
    while (x < size.width) {
        drawLine(c, Offset(x, 0f), Offset(x, size.height), strokeWidth = 1f)
        x += step
    }
    var y = 0f
    while (y < size.height) {
        drawLine(c, Offset(0f, y), Offset(size.width, y), strokeWidth = 1f)
        y += step
    }
}

// ---------------------------------------------------------------------------------------------
// Екран
// ---------------------------------------------------------------------------------------------

@Composable
fun TapeScreen(
    link: LinkState,
    deviceName: String?,
    status: TapeStatus,
    log: List<LogEntry>,
    bleSupported: Boolean,
    onConnectClick: () -> Unit,
    onCommand: (String) -> Unit,
) {
    Box(
        Modifier
            .fillMaxSize()
            .background(TC.Bg)
            .drawBehind { drawGrid() },
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .systemBarsPadding()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Column(Modifier.widthIn(max = 400.dp).fillMaxWidth()) {
                Header()
                Spacer(Modifier.height(14.dp))

                if (!bleSupported) {
                    NoBleBanner()
                    Spacer(Modifier.height(12.dp))
                }

                StatusBar(link, deviceName, onConnectClick)
                Spacer(Modifier.height(12.dp))

                Section("СТАН ПРИСТРОЮ") { Indicators(status) }
                Spacer(Modifier.height(12.dp))

                Section("ТРАНСПОРТ") { Transport(onCommand) }
                Spacer(Modifier.height(12.dp))

                Section("РЕЖИМИ") { Modes(status, onCommand) }
                Spacer(Modifier.height(12.dp))

                Section("ЛОГ") { LogView(log) }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Шапка, статус, секції
// ---------------------------------------------------------------------------------------------

@Composable
private fun Header() {
    val fonts = LocalFonts.current
    Box(Modifier.fillMaxWidth().panel()) {
        Box(
            Modifier
                .fillMaxWidth()
                .height(2.dp)
                .background(
                    Brush.horizontalGradient(listOf(Color.Transparent, TC.Accent, Color.Transparent)),
                ),
        )
        Column(
            Modifier.fillMaxWidth().padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            BasicText(
                "TAPE CONTROL",
                style = TextStyle(
                    fontFamily = fonts.display,
                    fontWeight = FontWeight.Black,
                    fontSize = 20.sp,
                    letterSpacing = 4.sp,
                    color = TC.Accent,
                    shadow = Shadow(TC.Accent.copy(alpha = 0.5f), blurRadius = 20f),
                ),
            )
            Spacer(Modifier.height(4.dp))
            BasicText("BLUETOOTH LE REMOTE", style = mono(9, 3f, TC.Dim))
        }
    }
}

@Composable
private fun NoBleBanner() {
    Box(Modifier.fillMaxWidth().border(1.dp, TC.Red).padding(14.dp), contentAlignment = Alignment.Center) {
        BasicText(
            "Bluetooth LE не підтримується на цьому пристрої.",
            style = mono(11, 0f, TC.Red).copy(textAlign = TextAlign.Center),
        )
    }
}

@Composable
private fun StatusBar(link: LinkState, deviceName: String?, onConnectClick: () -> Unit) {
    val connected = link == LinkState.CONNECTED
    val text = when (link) {
        LinkState.DISCONNECTED -> "НЕ ПІДКЛЮЧЕНО"
        LinkState.SCANNING -> "ПОШУК..."
        LinkState.CONNECTING -> "ПІДКЛЮЧЕННЯ..."
        LinkState.CONNECTED -> deviceName ?: "ПІДКЛЮЧЕНО"
    }
    val (label, danger) = when (link) {
        LinkState.DISCONNECTED -> "CONNECT" to false
        LinkState.SCANNING, LinkState.CONNECTING -> "CANCEL" to true
        LinkState.CONNECTED -> "DISCONNECT" to true
    }

    Row(
        Modifier.fillMaxWidth().panel().padding(horizontal = 14.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        StatusDot(connected)
        BasicText(
            text,
            modifier = Modifier.weight(1f),
            style = mono(10, 2f, if (connected) TC.Green else TC.Dim),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        ConnectButton(label, danger, onConnectClick)
    }
}

@Composable
private fun StatusDot(connected: Boolean) {
    // Пульсація (2 с) лише коли підключено
    val pulse: State<Float>? = if (connected) {
        rememberInfiniteTransition(label = "pulse").animateFloat(
            initialValue = 1f,
            targetValue = 0.5f,
            animationSpec = infiniteRepeatable(tween(1000, easing = LinearEasing), RepeatMode.Reverse),
            label = "pulseAlpha",
        )
    } else {
        null
    }
    Canvas(
        Modifier
            .size(8.dp)
            .graphicsLayer { alpha = pulse?.value ?: 1f },
    ) {
        val r = size.minDimension / 2f
        if (connected) {
            drawCircle(
                brush = Brush.radialGradient(
                    listOf(TC.Green.copy(alpha = 0.55f), Color.Transparent),
                    center = center,
                    radius = r * 2.8f,
                ),
                radius = r * 2.8f,
            )
            drawCircle(TC.Green, radius = r)
        } else {
            drawCircle(TC.Dim, radius = r)
        }
    }
}

@Composable
private fun ConnectButton(label: String, danger: Boolean, onClick: () -> Unit) {
    val src = remember { MutableInteractionSource() }
    val pressed by src.collectIsPressedAsState()
    val color = if (danger) TC.Red else TC.Accent
    val textColor = when {
        !pressed -> color
        danger -> Color.White
        else -> TC.Bg
    }
    Box(
        Modifier
            .border(1.dp, color)
            .background(if (pressed) color else Color.Transparent)
            .clickable(interactionSource = src, indication = null, onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 7.dp),
    ) {
        BasicText(
            label,
            style = TextStyle(
                fontFamily = LocalFonts.current.display,
                fontWeight = FontWeight.Bold,
                fontSize = 9.sp,
                letterSpacing = 2.sp,
                color = textColor,
            ),
        )
    }
}

@Composable
private fun Section(label: String, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth().panel()) {
        Box(
            Modifier
                .fillMaxWidth()
                .background(TC.Accent.copy(alpha = 0.03f))
                .padding(horizontal = 14.dp, vertical = 7.dp),
        ) {
            BasicText(label, style = mono(9, 3f, TC.Dim))
        }
        Box(Modifier.fillMaxWidth().height(1.dp).background(TC.Border))
        Column(Modifier.fillMaxWidth().padding(12.dp), content = content)
    }
}

// ---------------------------------------------------------------------------------------------
// Індикатори стану
// ---------------------------------------------------------------------------------------------

@Composable
private fun Indicators(s: TapeStatus) {
    val dBg = rgba(230, 81, 0, 0.12f)
    val dBorder = rgba(255, 107, 0, 0.25f)

    Row(
        Modifier.fillMaxWidth().height(IntrinsicSize.Min),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        IndCell(
            "REW", s.rew, rgba(21, 101, 192, 0.15f), rgba(21, 101, 192, 0.30f), TC.Accent,
            Glyphs.rew, Modifier.weight(1f).fillMaxHeight(),
        )
        IndCell(
            "STOP", s.stp, rgba(80, 80, 80, 0.20f), rgba(120, 120, 120, 0.30f), TC.Text,
            Glyphs.stop, Modifier.weight(1f).fillMaxHeight(),
        )
        IndCell(
            "PLAY", s.ply, rgba(46, 125, 50, 0.15f), rgba(76, 175, 80, 0.30f), TC.Green,
            Glyphs.play, Modifier.weight(1f).fillMaxHeight(),
        )
        IndCell(
            "FWD", s.fwd, rgba(106, 27, 154, 0.15f), rgba(142, 36, 170, 0.30f), TC.Accent,
            Glyphs.fwd, Modifier.weight(1f).fillMaxHeight(),
        )
    }
    Spacer(Modifier.height(6.dp))
    Row(
        Modifier.fillMaxWidth().height(IntrinsicSize.Min),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        IndCell(
            "REC", s.rec, rgba(198, 40, 40, 0.15f), rgba(229, 57, 53, 0.30f), TC.Red,
            Glyphs.rec, Modifier.weight(1f).fillMaxHeight(), blink = true,
        )
        IndCell("DOLBY", s.dlb, dBg, dBorder, TC.Accent2, null, Modifier.weight(1f).fillMaxHeight())
        IndCell("MPX", s.mpx, dBg, dBorder, TC.Accent2, null, Modifier.weight(1f).fillMaxHeight())
        IndCell("HX PRO", s.hx, dBg, dBorder, TC.Accent2, null, Modifier.weight(1f).fillMaxHeight())
    }

    // Активність ІЧ
    Row(
        Modifier.fillMaxWidth().padding(top = 8.dp),
        horizontalArrangement = Arrangement.End,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        BasicText("Активність IR", style = mono(10, 2f, TC.Dim))
        Spacer(Modifier.size(10.dp))
        Box(
            Modifier
                .size(10.dp)
                .background(if (s.ir) Color(0xFF2980B9) else Color(0xFF1A2A3A), CircleShape),
        )
    }
}

@Composable
private fun IndCell(
    label: String,
    on: Boolean,
    bg: Color,
    border: Color,
    onColor: Color,
    path: Path?,
    modifier: Modifier,
    blink: Boolean = false,
) {
    // Блимання REC (період 0.6 с) — анімація працює лише поки індикатор увімкнений
    val blinkAlpha: State<Float>? = if (on && blink) {
        rememberInfiniteTransition(label = "blink").animateFloat(
            initialValue = 1f,
            targetValue = 0.15f,
            animationSpec = infiniteRepeatable(tween(300, easing = LinearEasing), RepeatMode.Reverse),
            label = "blinkAlpha",
        )
    } else {
        null
    }
    val color = if (on) onColor else TC.Dim

    Column(
        modifier
            .background(bg)
            .border(1.dp, border)
            .padding(horizontal = 4.dp, vertical = 8.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(5.dp),
    ) {
        Box(Modifier.graphicsLayer { alpha = blinkAlpha?.value ?: 1f }) {
            if (path != null) GlyphIcon(path, color, 20.dp, glow = on) else LedDot(color, on)
        }
        BasicText(label, style = mono(8, 1f, color))
    }
}

@Composable
private fun GlyphIcon(path: Path, color: Color, iconSize: Dp, glow: Boolean) {
    Canvas(Modifier.size(iconSize)) {
        val k = size.width / 24f
        scale(k, k, pivot = Offset.Zero) {
            if (glow) {
                drawPath(path, color.copy(alpha = 0.35f), style = Stroke(width = 3f, join = StrokeJoin.Round))
            }
            drawPath(path, color)
        }
    }
}

@Composable
private fun LedDot(color: Color, on: Boolean) {
    Canvas(Modifier.size(10.dp)) {
        val r = size.minDimension / 2f
        if (on) {
            drawCircle(
                brush = Brush.radialGradient(
                    listOf(color.copy(alpha = 0.55f), Color.Transparent),
                    center = center,
                    radius = r * 2.4f,
                ),
                radius = r * 2.4f,
            )
        }
        drawCircle(color, radius = r)
    }
}

// ---------------------------------------------------------------------------------------------
// Транспорт
// ---------------------------------------------------------------------------------------------

@Composable
private fun Transport(onCommand: (String) -> Unit) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        TransportButton(
            "REW", Glyphs.rew, Color(0xFF1565C0), Color(0xFF1976D2), TC.Accent,
            Modifier.weight(1f),
        ) { onCommand("rew") }
        TransportButton(
            "STOP", Glyphs.stop, Color(0xFFC62828), Color(0xFFE53935), TC.Accent,
            Modifier.weight(1f),
        ) { onCommand("stop") }
        TransportButton(
            "PLAY", Glyphs.play, Color(0xFF2E7D32), Color(0xFF4CAF50), TC.Green,
            Modifier.weight(1f),
        ) { onCommand("play") }
        TransportButton(
            "FWD", Glyphs.fwd, Color(0xFF6A1B9A), Color(0xFF8E24AA), TC.Accent,
            Modifier.weight(1f),
        ) { onCommand("fwd") }
    }
}

@Composable
private fun TransportButton(
    label: String,
    path: Path,
    bg: Color,
    border: Color,
    pressedBorder: Color,
    modifier: Modifier,
    onClick: () -> Unit,
) {
    val src = remember { MutableInteractionSource() }
    val pressed by src.collectIsPressedAsState()
    val scaleAnim by animateFloatAsState(if (pressed) 0.95f else 1f, tween(150), label = "btnScale")
    val haptic = LocalHapticFeedback.current

    Column(
        modifier
            .aspectRatio(1f)
            .graphicsLayer {
                scaleX = scaleAnim
                scaleY = scaleAnim
            }
            .background(bg)
            .drawBehind { if (pressed) drawRect(TC.Accent.copy(alpha = 0.15f)) }
            .border(1.dp, if (pressed) pressedBorder else border)
            .clickable(interactionSource = src, indication = null) {
                haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                onClick()
            },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterVertically),
    ) {
        GlyphIcon(path, Color.White, 22.dp, glow = false)
        BasicText(label, style = mono(7, 1f, Color.White.copy(alpha = 0.7f)))
    }
}

// ---------------------------------------------------------------------------------------------
// Режими
// ---------------------------------------------------------------------------------------------

@Composable
private fun Modes(s: TapeStatus, onCommand: (String) -> Unit) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        ModeButton("🎵", "DOLBY", s.dlb, Modifier.weight(1f)) { onCommand("dolby") }
        ModeButton("📻", "MPX", s.mpx, Modifier.weight(1f)) { onCommand("mpx") }
        ModeButton("⚡", "HX PRO", s.hx, Modifier.weight(1f)) { onCommand("hx") }
    }
}

@Composable
private fun ModeButton(
    icon: String,
    label: String,
    active: Boolean,
    modifier: Modifier,
    onClick: () -> Unit,
) {
    val src = remember { MutableInteractionSource() }
    val pressed by src.collectIsPressedAsState()
    val scaleAnim by animateFloatAsState(if (pressed) 0.97f else 1f, tween(150), label = "modeScale")
    val haptic = LocalHapticFeedback.current
    val color = if (active) TC.Accent2 else TC.Dim

    Column(
        modifier
            .graphicsLayer {
                scaleX = scaleAnim
                scaleY = scaleAnim
            }
            .background(if (active) TC.Accent2.copy(alpha = 0.08f) else Color.Black.copy(alpha = 0.3f))
            .border(1.dp, if (active) TC.Accent2 else TC.Border)
            .clickable(interactionSource = src, indication = null) {
                haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                onClick()
            }
            .padding(horizontal = 8.dp, vertical = 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(5.dp),
    ) {
        BasicText(icon, style = TextStyle(fontSize = 16.sp))
        BasicText(label, style = mono(9, 1f, color))
    }
}

// ---------------------------------------------------------------------------------------------
// Лог
// ---------------------------------------------------------------------------------------------

@Composable
private fun LogView(log: List<LogEntry>) {
    val listState = rememberLazyListState()
    // Автопрокрутка до останнього запису
    LaunchedEffect(log.lastOrNull()?.id) {
        if (log.isNotEmpty()) listState.scrollToItem(log.lastIndex)
    }
    LazyColumn(Modifier.fillMaxWidth().height(65.dp), state = listState) {
        items(log, key = { it.id }) { e ->
            val msgColor = when (e.type) {
                LogType.OK -> TC.Green
                LogType.ERR -> TC.Red
                LogType.INFO -> TC.Text
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                BasicText(
                    e.time,
                    style = mono(10, 0f, TC.Accent.copy(alpha = 0.5f)).copy(lineHeight = 16.sp),
                )
                BasicText(e.msg, style = mono(10, 0f, msgColor).copy(lineHeight = 16.sp))
            }
        }
    }
}
