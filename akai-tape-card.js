/**
 * Akai GX-747 Tape Deck Card for Home Assistant
 * Custom Element — отримує hass автоматично, токени не потрібні
 */
class AkaiTapeCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this._hass = null;
    this._config = {};
    this._initialized = false;
  }

  setConfig(config) {
    this._config = config;
    this._initialized = false;
    this._render();
  }

  set hass(hass) {
    this._hass = hass;
    if (!this._initialized) {
      this._render();
    } else {
      this._update();
    }
  }

  getCardSize() { return 8; }

  _e(id) { return this.shadowRoot.getElementById(id); }

  _state(eid) {
    if (!this._hass || !eid) return null;
    const s = this._hass.states[eid];
    return s ? s.state : null;
  }

  _callService(domain, service, entity_id) {
    if (!this._hass) return;
    this._hass.callService(domain, service, { entity_id });
  }

  _render() {
    this.shadowRoot.innerHTML = `
      <style>${this._css()}</style>
      <ha-card>
        <div class="akai-body">
          ${this._htmlTop()}
          ${this._htmlVU()}
          ${this._htmlModes()}
          ${this._htmlTransport()}
        </div>
      </ha-card>
    `;
    this._bindEvents();
    this._initialized = true;
    // Даємо DOM час відмалюватись перед update
    setTimeout(() => {
      if (this._hass) this._update();
      this._startAnimation();
    }, 100);
  }

  _css() {
    return `
      :host { display: block; }

      .akai-body {
        background: linear-gradient(180deg, #2a2a2a 0%, #1c1c1c 30%, #151515 100%);
        border-radius: 14px;
        overflow: hidden;
        min-height: 200px;
        font-family: 'Arial Narrow', Arial, sans-serif;
        box-shadow:
          0 10px 40px rgba(0,0,0,0.9),
          inset 0 1px 0 rgba(255,255,255,0.08),
          inset 0 -1px 0 rgba(0,0,0,0.5);
        border: 1px solid #111;
        user-select: none;
      }

      /* ═══ TOP — КОТУШКИ + ГОЛІВКИ ═══ */
      .top-section {
        background: linear-gradient(180deg, #c8c8c8 0%, #a0a0a0 40%, #888 100%);
        padding: 12px 8px 8px;
        position: relative;
        border-bottom: 3px solid #555;
      }

      .top-section::before {
        content: '';
        position: absolute;
        top: 0; left: 0; right: 0;
        height: 2px;
        background: linear-gradient(90deg, #999, #ddd, #bbb, #ddd, #999);
      }

      .reels-row {
        display: flex;
        justify-content: space-between;
        align-items: flex-end;
        gap: 8px;
      }

      /* Котушка */
      .reel-wrap {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 4px;
      }

      .reel-svg-wrap {
        filter: drop-shadow(0 4px 8px rgba(0,0,0,0.5));
      }

      .reel-label {
        font-size: 9px;
        color: #444;
        letter-spacing: 1px;
        font-weight: bold;
      }

      /* Блок голівок по центру */
      .head-block {
        flex: 1;
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 6px;
        padding-bottom: 4px;
      }

      .counter-box {
        background: #1a1a1a;
        border: 1px solid #333;
        border-radius: 4px;
        padding: 4px 10px;
        text-align: center;
        box-shadow: inset 0 2px 6px rgba(0,0,0,0.8);
        min-width: 80px;
      }

      .counter-label {
        font-size: 7px;
        color: #444;
        letter-spacing: 2px;
      }

      .counter-val {
        font-family: 'Courier New', monospace;
        font-size: 18px;
        font-weight: bold;
        color: #ff6600;
        text-shadow: 0 0 8px rgba(255,100,0,0.8);
        letter-spacing: 3px;
      }

      .tape-path-svg {
        filter: drop-shadow(0 2px 4px rgba(0,0,0,0.4));
      }

      /* Бренд */
      .brand-row {
        display: flex;
        justify-content: space-between;
        align-items: center;
        padding: 4px 8px 0;
      }

      .brand-name {
        font-family: 'Arial', sans-serif;
        font-size: 13px;
        font-weight: 900;
        color: #222;
        letter-spacing: 4px;
        font-style: italic;
      }

      .model-name {
        font-size: 8px;
        color: #555;
        letter-spacing: 2px;
      }

      /* ═══ VU МЕТРИ ═══ */
      .vu-section {
        background: linear-gradient(180deg, #1a1a1a, #111);
        padding: 10px 10px 6px;
        display: flex;
        gap: 8px;
        border-bottom: 1px solid #333;
      }

      .vu-meter {
        flex: 1;
        background: linear-gradient(180deg, #f5e6c0, #e8d090);
        border-radius: 6px;
        padding: 6px;
        position: relative;
        box-shadow:
          inset 0 2px 6px rgba(0,0,0,0.3),
          0 2px 8px rgba(0,0,0,0.5);
        border: 1px solid #c8b060;
      }

      .vu-meter canvas {
        display: block;
        width: 100%;
      }

      .vu-ch-label {
        position: absolute;
        bottom: 5px;
        left: 8px;
        font-size: 9px;
        font-weight: bold;
        color: #333;
        letter-spacing: 1px;
      }

      .vu-ch-label.right {
        left: auto;
        right: 8px;
      }

      /* ═══ РЕЖИМИ ═══ */
      .modes-section {
        background: #111;
        padding: 8px 10px;
        display: flex;
        gap: 6px;
        justify-content: center;
        border-bottom: 1px solid #2a2a2a;
      }

      .mode-btn {
        flex: 1;
        max-width: 80px;
        padding: 7px 4px;
        border-radius: 5px;
        background: linear-gradient(180deg, #2a2a2a, #1a1a1a);
        border: 1px solid #333;
        color: #666;
        font-size: 10px;
        font-weight: bold;
        letter-spacing: 1px;
        text-align: center;
        cursor: pointer;
        transition: all 0.15s;
        box-shadow: 0 2px 4px rgba(0,0,0,0.5), inset 0 1px 0 rgba(255,255,255,0.05);
      }

      .mode-btn.on {
        background: linear-gradient(180deg, #3a3020, #1a1505);
        border-color: #c8a85a;
        color: #c8a85a;
        box-shadow: 0 0 8px rgba(200,168,90,0.3), 0 2px 4px rgba(0,0,0,0.5);
      }

      .mode-btn .mode-icon { font-size: 14px; display: block; margin-bottom: 2px; }

      /* ═══ ТРАНСПОРТ ═══ */
      .transport-section {
        background: linear-gradient(180deg, #111, #0d0d0d);
        padding: 10px 10px 12px;
        display: flex;
        gap: 6px;
      }

      .t-btn {
        flex: 1;
        padding: 14px 4px;
        border-radius: 8px;
        background: linear-gradient(180deg, #303030, #1a1a1a);
        border: 1px solid #3a3a3a;
        border-bottom: 3px solid #111;
        cursor: pointer;
        transition: all 0.1s;
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 4px;
        box-shadow:
          0 4px 8px rgba(0,0,0,0.6),
          inset 0 1px 0 rgba(255,255,255,0.08);
      }

      .t-btn:active {
        transform: translateY(2px);
        border-bottom-width: 1px;
        box-shadow: 0 2px 4px rgba(0,0,0,0.6);
      }

      .t-btn.active {
        background: linear-gradient(180deg, #1a3a20, #0d2010);
        border-color: #00aa44;
        border-bottom-color: #006622;
        box-shadow: 0 0 12px rgba(0,200,68,0.4), 0 4px 8px rgba(0,0,0,0.6);
      }

      .t-btn.stop-btn {
        background: linear-gradient(180deg, #3a1a1a, #1a0d0d);
        border-color: #aa2200;
      }

      .t-btn-icon {
        font-size: 20px;
        color: #c8a85a;
        line-height: 1;
      }

      .t-btn.active .t-btn-icon { color: #00ff88; }
      .t-btn.stop-btn .t-btn-icon { color: #ff4444; }

      .t-btn-label {
        font-size: 8px;
        color: #555;
        letter-spacing: 1px;
        font-weight: bold;
      }

      .t-btn.active .t-btn-label { color: #00aa44; }

      /* Анімація котушок */
      .reel-spokes {
        transform-origin: 50% 50%;
        transform-box: fill-box;
      }
      .reel-spokes.spin { animation: spin 2s linear infinite; }
      .reel-spokes.spin-fast { animation: spin 0.5s linear infinite; }
      .reel-spokes.spin-rev { animation: spin-rev 0.5s linear infinite; }

      @keyframes spin { from{transform:rotate(0deg)} to{transform:rotate(360deg)} }
      @keyframes spin-rev { from{transform:rotate(0deg)} to{transform:rotate(-360deg)} }
    `;
  }

  _reelSVG(id, size, tapeThick) {
    const cx = size/2, cy = size/2;
    const R = size/2 - 2;
    const hubR = size * 0.14;
    const spokeR = size * 0.32;
    const tapeR = R - tapeThick;

    let spokes = '';
    for (let i = 0; i < 3; i++) {
      const a = (i * 120) * Math.PI / 180;
      const x1 = cx + Math.cos(a) * hubR;
      const y1 = cy + Math.sin(a) * hubR;
      const x2 = cx + Math.cos(a) * spokeR;
      const y2 = cy + Math.sin(a) * spokeR;
      spokes += `
        <line x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}"
              x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}"
              stroke="#222" stroke-width="${(size*0.06).toFixed(1)}" stroke-linecap="round"/>`;
    }

    return `<svg width="${size}" height="${size}" viewBox="0 0 ${size} ${size}" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <radialGradient id="${id}rg" cx="40%" cy="35%" r="65%">
          <stop offset="0%" stop-color="#f0f0f0"/>
          <stop offset="60%" stop-color="#d0d0d0"/>
          <stop offset="100%" stop-color="#a0a0a0"/>
        </radialGradient>
        <radialGradient id="${id}hg" cx="35%" cy="30%" r="70%">
          <stop offset="0%" stop-color="#555"/>
          <stop offset="100%" stop-color="#1a1a1a"/>
        </radialGradient>
      </defs>
      <!-- Зовнішній обід -->
      <circle cx="${cx}" cy="${cy}" r="${R}" fill="url(#${id}rg)" stroke="#888" stroke-width="1.5"/>
      <!-- Плівка -->
      <circle cx="${cx}" cy="${cy}" r="${tapeR}" fill="#3a2a1a" stroke="#555" stroke-width="0.5"/>
      <!-- Спиці -->
      <g id="${id}-spokes" class="reel-spokes">${spokes}</g>
      <!-- Ступиця -->
      <circle cx="${cx}" cy="${cy}" r="${hubR}" fill="url(#${id}hg)" stroke="#333" stroke-width="1"/>
      <circle cx="${cx}" cy="${cy}" r="${hubR*0.4}" fill="#111"/>
      <!-- Блик -->
      <ellipse cx="${cx-R*0.2}" cy="${cy-R*0.2}" rx="${R*0.3}" ry="${R*0.15}"
               fill="rgba(255,255,255,0.25)" transform="rotate(-30 ${cx} ${cy})"/>
    </svg>`;
  }

  _htmlTop() {
    const reelSize = 90;
    return `
    <div class="top-section">
      <div class="reels-row">
        <!-- Ліва котушка -->
        <div class="reel-wrap">
          <div class="reel-svg-wrap" id="reel-left-wrap">
            ${this._reelSVG('L', reelSize, 14)}
          </div>
          <div class="reel-label">SUPPLY</div>
        </div>

        <!-- Центральний блок: лічильник + голівки -->
        <div class="head-block">
          <div class="counter-box">
            <div class="counter-label">COUNTER</div>
            <div class="counter-val" id="counter-val">00:00</div>
          </div>
          <svg class="tape-path-svg" width="80" height="40" viewBox="0 0 80 40">
            <defs>
              <linearGradient id="tpg" x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stop-color="#888"/>
                <stop offset="100%" stop-color="#555"/>
              </linearGradient>
            </defs>
            <!-- Направляючі ролики -->
            <circle cx="8" cy="20" r="5" fill="#444" stroke="#666" stroke-width="1"/>
            <circle cx="72" cy="20" r="5" fill="#444" stroke="#666" stroke-width="1"/>
            <!-- Голівки блок -->
            <rect x="25" y="10" width="30" height="20" rx="3" fill="url(#tpg)" stroke="#666" stroke-width="1"/>
            <rect x="29" y="14" width="6" height="12" rx="1" fill="#333"/>
            <rect x="37" y="14" width="6" height="12" rx="1" fill="#333"/>
            <rect x="45" y="14" width="6" height="12" rx="1" fill="#333"/>
            <!-- Стрічка -->
            <path d="M13,20 Q20,18 25,20" fill="none" stroke="#2a2010" stroke-width="2"/>
            <path d="M55,20 Q60,18 67,20" fill="none" stroke="#2a2010" stroke-width="2"/>
          </svg>
        </div>

        <!-- Права котушка -->
        <div class="reel-wrap">
          <div class="reel-svg-wrap" id="reel-right-wrap">
            ${this._reelSVG('R', reelSize, 8)}
          </div>
          <div class="reel-label">TAKE-UP</div>
        </div>
      </div>

      <div class="brand-row">
        <div class="brand-name">AKAI</div>
        <div class="model-name">GX-747 · PIONEER CT-737</div>
      </div>
    </div>`;
  }

  _htmlVU() {
    return `
    <div class="vu-section">
      <div class="vu-meter">
        <canvas id="vu-canvas-l" height="60"></canvas>
        <div class="vu-ch-label">L</div>
        <div class="vu-ch-label right">VU</div>
      </div>
      <div class="vu-meter">
        <canvas id="vu-canvas-r" height="60"></canvas>
        <div class="vu-ch-label">R</div>
        <div class="vu-ch-label right">VU</div>
      </div>
    </div>`;
  }

  _htmlModes() {
    return `
    <div class="modes-section">
      <div class="mode-btn" id="mode-play">
        <span class="mode-icon">▶</span>PLAY
      </div>
      <div class="mode-btn" id="mode-rew">
        <span class="mode-icon">⏮</span>REW
      </div>
      <div class="mode-btn" id="mode-fwd">
        <span class="mode-icon">⏭</span>FWD
      </div>
      <div class="mode-btn" id="mode-pau">
        <span class="mode-icon">⏸</span>PAUSE
      </div>
      <div class="mode-btn" id="mode-dolby">
        <span class="mode-icon">〽</span>
        <span id="dolby-val">DOLBY</span>
      </div>
      <div class="mode-btn" id="mode-mpx">
        <span class="mode-icon">📻</span>MPX
      </div>
      <div class="mode-btn" id="mode-rssi">
        <span class="mode-icon">📶</span>
        <span id="rssi-val">—</span>
      </div>
    </div>`;
  }

  _htmlTransport() {
    return `
    <div class="transport-section">
      <button class="t-btn" id="btn-rew">
        <span class="t-btn-icon">⏮</span>
        <span class="t-btn-label">REW</span>
      </button>
      <button class="t-btn" id="btn-play">
        <span class="t-btn-icon">▶</span>
        <span class="t-btn-label">PLAY</span>
      </button>
      <button class="t-btn stop-btn" id="btn-stop">
        <span class="t-btn-icon">■</span>
        <span class="t-btn-label">STOP</span>
      </button>
      <button class="t-btn" id="btn-fwd">
        <span class="t-btn-icon">⏭</span>
        <span class="t-btn-label">FWD</span>
      </button>
    </div>`;
  }

  _bindEvents() {
    // Кнопки транспорту — точні entity ID з прошивки
    const btns = {
      'btn-play': 'button.tape_deck_tape_play',
      'btn-stop': 'button.tape_deck_tape_stop',
      'btn-rew':  'button.tape_deck_tape_rew',
      'btn-fwd':  'button.tape_deck_tape_fwd'
    };
    Object.entries(btns).forEach(([id, eid]) => {
      const btn = this._e(id);
      if (btn) btn.addEventListener('click', () => {
        this._callService('button', 'press', eid);
        btn.style.transform = 'translateY(2px)';
        setTimeout(() => btn.style.transform = '', 150);
      });
    });
  }

  _drawVU(canvas, level, channel) {
    if (!canvas) return;
    const W = canvas.offsetWidth || 140;
    const H = 60;
    canvas.width = W;
    canvas.height = H;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    ctx.clearRect(0, 0, W, H);

    // Фон циферблату
    const cx = W/2, cy = H + 10;
    const R = H * 1.4;

    // Дуга шкали
    const startA = Math.PI * 1.1;
    const endA = Math.PI * 1.9;

    // Фон дуги
    ctx.beginPath();
    ctx.arc(cx, cy, R, startA, endA);
    ctx.strokeStyle = '#ccc';
    ctx.lineWidth = 2;
    ctx.stroke();

    // Мітки шкали
    const marks = [
      {v:0, label:'-20'}, {v:0.25, label:'-10'}, {v:0.45, label:'-7'},
      {v:0.6, label:'-3'}, {v:0.75, label:'0'}, {v:0.85, label:'+3'}
    ];

    ctx.font = '7px Arial';
    ctx.fillStyle = '#333';
    ctx.textAlign = 'center';

    marks.forEach(m => {
      const a = startA + m.v * (endA - startA);
      const x1 = cx + Math.cos(a) * (R - 4);
      const y1 = cy + Math.sin(a) * (R - 4);
      const x2 = cx + Math.cos(a) * (R - 10);
      const y2 = cy + Math.sin(a) * (R - 10);

      ctx.beginPath();
      ctx.moveTo(x1, y1);
      ctx.lineTo(x2, y2);
      ctx.strokeStyle = m.v >= 0.75 ? '#cc4400' : '#444';
      ctx.lineWidth = 1;
      ctx.stroke();

      const xt = cx + Math.cos(a) * (R - 16);
      const yt = cy + Math.sin(a) * (R - 16);
      ctx.fillStyle = m.v >= 0.75 ? '#cc4400' : '#333';
      ctx.fillText(m.label, xt, yt + 3);
    });

    // Стрілка
    const needleA = startA + level * (endA - startA);
    const nx = cx + Math.cos(needleA) * (R - 6);
    const ny = cy + Math.sin(needleA) * (R - 6);
    const bx = cx + Math.cos(needleA + Math.PI) * 8;
    const by = cy + Math.sin(needleA + Math.PI) * 8;

    ctx.beginPath();
    ctx.moveTo(bx, by);
    ctx.lineTo(nx, ny);
    ctx.strokeStyle = '#111';
    ctx.lineWidth = 1.5;
    ctx.stroke();

    // Pivot
    ctx.beginPath();
    ctx.arc(cx, cy, 5, 0, Math.PI * 2);
    ctx.fillStyle = '#333';
    ctx.fill();
    ctx.strokeStyle = '#555';
    ctx.lineWidth = 1;
    ctx.stroke();
  }

  _update() {
    if (!this._hass) return;

    // Лічильник
    const ctr = this._state('sensor.tape_deck_counter');
    if (ctr) {
      const v = parseInt(ctr) || 0;
      const abs = Math.abs(v);
      const m = Math.floor(abs/60);
      const s = abs%60;
      const el = this._e('counter-val');
      if (el) el.textContent = (v<0?'-':'') + String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0');
    }

    // VU метри
    const vuL = parseFloat(this._state('sensor.tape_deck_vu_l')) || 0;
    const vuR = parseFloat(this._state('sensor.tape_deck_vu_r')) || 0;
    const levL = Math.min(1, Math.max(0, vuL/16));
    const levR = Math.min(1, Math.max(0, vuR/16));

    this._drawVU(this._e('vu-canvas-l'), levL, 'L');
    this._drawVU(this._e('vu-canvas-r'), levR, 'R');

    // Стан транспорту
    const play = this._state('binary_sensor.tape_deck_vfd_ply') === 'on';
    const rew  = this._state('binary_sensor.tape_deck_vfd_rew') === 'on';
    const fwd  = this._state('binary_sensor.tape_deck_vfd_fwd') === 'on';
    const pau  = this._state('binary_sensor.tape_deck_vfd_pau') === 'on';

    const toggle = (id, active) => {
      const el = this._e(id);
      if (el) el.classList.toggle('active', active);
    };
    const toggleMode = (id, active) => {
      const el = this._e(id);
      if (el) el.classList.toggle('on', active);
    };

    // Кнопки транспорту
    toggle('btn-play', play);
    toggle('btn-rew', rew);
    toggle('btn-fwd', fwd);

    // Котушки
    const leftSpokes  = this._e('reel-left-wrap')?.querySelector('.reel-spokes');
    const rightSpokes = this._e('reel-right-wrap')?.querySelector('.reel-spokes');
    if (leftSpokes && rightSpokes) {
      leftSpokes.className  = 'reel-spokes';
      rightSpokes.className = 'reel-spokes';
      if (play) { leftSpokes.classList.add('spin'); rightSpokes.classList.add('spin'); }
      else if (rew) { leftSpokes.classList.add('spin-rev'); rightSpokes.classList.add('spin-fast'); }
      else if (fwd) { leftSpokes.classList.add('spin-fast'); rightSpokes.classList.add('spin-rev'); }
    }

    // Індикатори режимів
    toggleMode('mode-play', play);
    toggleMode('mode-rew',  rew);
    toggleMode('mode-fwd',  fwd);
    toggleMode('mode-pau',  pau);

    // Dolby
    const dolby = this._state('sensor.tape_deck_dolby');
    const dolbyActive = dolby && dolby !== '0' && dolby !== 'Вимк';
    toggleMode('mode-dolby', dolbyActive);
    const dolbyEl = this._e('dolby-val');
    if (dolbyEl) dolbyEl.textContent = dolbyActive ? dolby : 'DOLBY';

    // MPX
    const mpx = this._state('sensor.tape_deck_mpx');
    toggleMode('mode-mpx', mpx === 'on' || mpx === 'ON' || mpx === '1');

    // RSSI
    const rssi = this._state('sensor.tape_deck_rssi');
    const rssiEl = this._e('rssi-val');
    if (rssiEl) rssiEl.textContent = rssi ? rssi + 'dB' : '—';
  }

  _startAnimation() {
    // Оновлення VU кожні 200мс для плавності
    this._animInterval = setInterval(() => {
      if (this._hass) {
        const vuL = parseFloat(this._state('sensor.tape_deck_vu_l')) || 0;
        const vuR = parseFloat(this._state('sensor.tape_deck_vu_r')) || 0;
        this._drawVU(this._e('vu-canvas-l'), Math.min(1, vuL/16));
        this._drawVU(this._e('vu-canvas-r'), Math.min(1, vuR/16));
      }
    }, 200);
  }

  disconnectedCallback() {
    if (this._animInterval) clearInterval(this._animInterval);
  }
}

customElements.define('akai-tape-card', AkaiTapeCard);
