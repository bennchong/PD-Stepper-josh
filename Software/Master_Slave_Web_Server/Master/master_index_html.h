const char master_index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>PD Stepper Master/Slave</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    html { font-family: Arial, Helvetica, sans-serif; display: inline-block; text-align: center; }
    body { margin: 0; background-color: #212121; color: #efefef; }
    h1   { font-size: 1.6rem; color: #efefef; margin-bottom: 6px; }
    h2   { font-size: 1.2rem; margin: 12px 0 6px; }
    h3   { font-size: 1.0rem; margin: 14px 0 4px; }
    hr   { border: 0; border-top: 1px solid #444; margin: 10px 0; }
    a    { color: #fc4903; }

    /* ── Two-panel layout ─────────────────────────────────────── */
    .panels {
      display: flex;
      flex-wrap: wrap;
      gap: 20px;
      justify-content: center;
      padding: 14px 10px 30px;
    }
    .panel {
      background-color: #2a2a2a;
      border-radius: 10px;
      padding: 16px 18px 20px;
      width: 440px;
      box-sizing: border-box;
    }
    .panel-master h1 { color: #fc4903; }
    .panel-slave  h1 { color: #4f9cf8; }

    /* ── Live stats ───────────────────────────────────────────── */
    .stat-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 8px 6px;
      text-align: left;
      margin-bottom: 10px;
    }
    .stat-item { background-color: #1a1a1a; border-radius: 6px; padding: 6px 10px; font-size: 0.85rem; }
    .stat-label { color: #aaa; font-size: 0.75rem; }
    .stat-value { color: #efefef; font-weight: bold; }

    /* ── Connection badge ─────────────────────────────────────── */
    #slave-connected {
      display: inline-block;
      padding: 3px 10px;
      border-radius: 12px;
      font-size: 0.8rem;
      font-weight: bold;
      background-color: #555;
      color: #fff;
    }
    #slave-connected.ok  { background-color: #2e7d32; }
    #slave-connected.bad { background-color: #c62828; }

    /* ── Settings form ────────────────────────────────────────── */
    details { text-align: left; margin-top: 10px; }
    details summary { cursor: pointer; font-size: 0.9rem; color: #aaa; padding: 4px 0; user-select: none; }
    .form-row { display: flex; align-items: center; justify-content: space-between; margin: 6px 0; font-size: 0.85rem; }
    .form-row label { color: #bbb; }
    select, input[type=number] {
      background: #1a1a1a;
      color: #efefef;
      border: 1px solid #555;
      border-radius: 4px;
      padding: 3px 6px;
      font-size: 0.83rem;
    }
    input[type=checkbox] { width: 16px; height: 16px; accent-color: #fc4903; cursor: pointer; }
    .panel-slave  input[type=checkbox] { accent-color: #4f9cf8; }
    input[type=submit], button.save-btn {
      margin-top: 10px;
      width: 100%;
      background-color: #fc4903;
      color: white;
      border: none;
      border-radius: 6px;
      padding: 8px;
      font-size: 0.9rem;
      cursor: pointer;
    }
    .panel-slave input[type=submit], .panel-slave button.save-btn { background-color: #4f9cf8; }
    input[type=submit]:hover, button.save-btn:hover { opacity: 0.85; }

    /* ── Velocity slider ──────────────────────────────────────── */
    .slider-wrap { display: flex; align-items: center; gap: 10px; margin: 8px 0; }
    input[type=range] {
      flex: 1;
      -webkit-appearance: none;
      appearance: none;
      height: 8px;
      border-radius: 4px;
      background: #555;
      outline: none;
    }
    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 20px; height: 20px;
      border-radius: 50%;
      background: #fc4903;
      cursor: pointer;
    }
    .panel-slave input[type=range]::-webkit-slider-thumb { background: #4f9cf8; }
    .toggle-label { display: flex; align-items: center; gap: 6px; font-size: 0.8rem; color: #aaa; }

    /* ── Position control buttons ─────────────────────────────── */
    .pos-btns { display: flex; gap: 8px; margin: 8px 0; }
    .pos-btns button {
      flex: 1;
      background-color: #333;
      color: #efefef;
      border: 1px solid #555;
      border-radius: 6px;
      padding: 8px 4px;
      font-size: 1.1rem;
      cursor: pointer;
    }
    .pos-btns button:hover { background-color: #444; }

    /* ── Logo ─────────────────────────────────────────────────── */
    .logo-wrap { margin: 8px 0 4px; }
    .logo-wrap img { max-width: 200px; height: auto; }
  </style>
</head>
<body>

<div class="panels">

  <!-- ════════════════ MASTER PANEL ════════════════ -->
  <div class="panel panel-master">
    <h1>&#9881; MASTER MOTOR</h1>
    <hr>

    <!-- Live stats -->
    <h3>Live Stats</h3>
    <div class="stat-grid">
      <div class="stat-item"><div class="stat-label">PD Voltage</div><div class="stat-value" id="voltage">--</div></div>
      <div class="stat-item"><div class="stat-label">USB PD Status</div><div class="stat-value" id="powergood">--</div></div>
      <div class="stat-item"><div class="stat-label">Driver Status</div><div class="stat-value" id="status">--</div></div>
      <div class="stat-item"><div class="stat-label">Encoder Position</div><div class="stat-value" id="position">--</div></div>
      <div class="stat-item"><div class="stat-label">StallGuard</div><div class="stat-value" id="stallguard">--</div></div>
    </div>

    <!-- Settings (collapsible) -->
    <details>
      <summary>&#9881; Settings</summary>
      <form action="/save" method="post">
        <div class="form-row">
          <label>Enable</label>
          <input type="checkbox" name="enabled1" %enabled1%>
        </div>
        <div class="form-row">
          <label>USB-PD Voltage</label>
          <select name="setvoltage" id="setvoltage">
            <option value="5">5V</option>
            <option value="9">9V</option>
            <option value="12">12V</option>
            <option value="15">15V</option>
            <option value="20">20V</option>
          </select>
        </div>
        <div class="form-row">
          <label>Microsteps</label>
          <select name="microsteps" id="microsteps">
            <option value="1">1</option>
            <option value="2">2</option>
            <option value="4">4</option>
            <option value="8">8</option>
            <option value="16">16</option>
            <option value="32">32</option>
            <option value="64">64</option>
            <option value="128">128</option>
            <option value="256">256</option>
          </select>
        </div>
        <div class="form-row">
          <label>Run Current (%)</label>
          <select name="current" id="current">
            <option value="10">10</option>
            <option value="20">20</option>
            <option value="30">30</option>
            <option value="40">40</option>
            <option value="50">50</option>
            <option value="60">60</option>
            <option value="70">70</option>
            <option value="80">80</option>
            <option value="90">90</option>
            <option value="100">100</option>
          </select>
        </div>
        <div class="form-row">
          <label>Stall Threshold</label>
          <select name="stall_threshold" id="stall_threshold">
            <option value="0">0</option>
            <option value="5">5</option>
            <option value="10">10</option>
            <option value="20">20</option>
            <option value="30">30</option>
            <option value="40">40</option>
            <option value="50">50</option>
          </select>
        </div>
        <div class="form-row">
          <label>Standstill Mode</label>
          <select name="standstill_mode" id="standstill_mode">
            <option value="NORMAL">NORMAL</option>
            <option value="FREEWHEELING">FREEWHEELING</option>
            <option value="BRAKING">BRAKING</option>
            <option value="STRONG_BRAKING">STRONG BRAKING</option>
          </select>
        </div>
        <input type="submit" value="Save Master Settings">
      </form>
    </details>

    <!-- Velocity control -->
    <h3>Velocity Control</h3>
    <div class="slider-wrap">
      <span>&#9664;</span>
      <input type="range" id="slider" min="-330" max="330" value="0"
             oninput="throttledUpdate()" onchange="checkReset()">
      <span>&#9654;</span>
    </div>
    <label class="toggle-label">
      <input type="checkbox" id="toggleReset" onchange="toggleReset()"> Auto-centre on release
    </label>

    <!-- Position control -->
    <h3>Position Control</h3>
    <div class="pos-btns">
      <button onclick="but1()">&#171;&#171;</button>
      <button onclick="but2()">&#171;</button>
      <button onclick="but3()">&#187;</button>
      <button onclick="but4()">&#187;&#187;</button>
    </div>

    <!-- Logo -->
    <div class="logo-wrap">
      <img src="data:image/png;base64,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" alt="PD Stepper">
    </div>

    <p style="font-size:0.7rem;color:#666;margin-top:6px;">
      <a href="https://thingsbyjosh.com" target="_blank">thingsbyjosh.com</a>
    </p>
  </div><!-- end master panel -->

  <!-- ════════════════ SLAVE PANEL ════════════════ -->
  <div class="panel panel-slave">
    <h1>&#128279; SLAVE MOTOR</h1>
    <hr>

    <!-- Connection status -->
    <p style="margin:4px 0 10px;">
      Connection:&nbsp;<span id="slave-connected">Checking&hellip;</span>
    </p>

    <!-- Live stats -->
    <h3>Live Stats</h3>
    <div class="stat-grid">
      <div class="stat-item"><div class="stat-label">PD Voltage</div><div class="stat-value" id="slave-voltage">--</div></div>
      <div class="stat-item"><div class="stat-label">USB PD Status</div><div class="stat-value" id="slave-powergood">--</div></div>
      <div class="stat-item"><div class="stat-label">Driver Status</div><div class="stat-value" id="slave-status">--</div></div>
      <div class="stat-item"><div class="stat-label">Encoder Position</div><div class="stat-value" id="slave-position">--</div></div>
    </div>

    <!-- Settings (collapsible) -->
    <details>
      <summary>&#9881; Settings</summary>
      <form action="/slave/save" method="post">
        <div class="form-row">
          <label>Enable</label>
          <input type="checkbox" name="enabled1" checked>
        </div>
        <div class="form-row">
          <label>USB-PD Voltage</label>
          <select name="setvoltage" id="slave-setvoltage">
            <option value="5">5V</option>
            <option value="9">9V</option>
            <option value="12" selected>12V</option>
            <option value="15">15V</option>
            <option value="20">20V</option>
          </select>
        </div>
        <div class="form-row">
          <label>Microsteps</label>
          <select name="microsteps" id="slave-microsteps">
            <option value="1">1</option>
            <option value="2">2</option>
            <option value="4">4</option>
            <option value="8">8</option>
            <option value="16">16</option>
            <option value="32" selected>32</option>
            <option value="64">64</option>
            <option value="128">128</option>
            <option value="256">256</option>
          </select>
        </div>
        <div class="form-row">
          <label>Run Current (%)</label>
          <select name="current" id="slave-current">
            <option value="10">10</option>
            <option value="20">20</option>
            <option value="30" selected>30</option>
            <option value="40">40</option>
            <option value="50">50</option>
            <option value="60">60</option>
            <option value="70">70</option>
            <option value="80">80</option>
            <option value="90">90</option>
            <option value="100">100</option>
          </select>
        </div>
        <div class="form-row">
          <label>Stall Threshold</label>
          <select name="stall_threshold" id="slave-stall_threshold">
            <option value="0">0</option>
            <option value="5">5</option>
            <option value="10" selected>10</option>
            <option value="20">20</option>
            <option value="30">30</option>
            <option value="40">40</option>
            <option value="50">50</option>
          </select>
        </div>
        <div class="form-row">
          <label>Standstill Mode</label>
          <select name="standstill_mode" id="slave-standstill_mode">
            <option value="NORMAL" selected>NORMAL</option>
            <option value="FREEWHEELING">FREEWHEELING</option>
            <option value="BRAKING">BRAKING</option>
            <option value="STRONG_BRAKING">STRONG BRAKING</option>
          </select>
        </div>
        <input type="submit" value="Save Slave Settings">
      </form>
    </details>

    <!-- Velocity control -->
    <h3>Velocity Control</h3>
    <div class="slider-wrap">
      <span>&#9664;</span>
      <input type="range" id="slave-slider" min="-330" max="330" value="0"
             oninput="slaveThrottledUpdate()" onchange="slaveCheckReset()">
      <span>&#9654;</span>
    </div>
    <label class="toggle-label">
      <input type="checkbox" id="slaveToggleReset" onchange="slaveToggleReset()"> Auto-centre on release
    </label>

    <!-- Position control -->
    <h3>Position Control</h3>
    <div class="pos-btns">
      <button onclick="slaveBut1()">&#171;&#171;</button>
      <button onclick="slaveBut2()">&#171;</button>
      <button onclick="slaveBut3()">&#187;</button>
      <button onclick="slaveBut4()">&#187;&#187;</button>
    </div>

    <p style="font-size:0.75rem;color:#555;margin-top:16px;">
      Slave controls are forwarded by the master over WiFi.
    </p>
  </div><!-- end slave panel -->

</div><!-- end .panels -->

<script>
  /* ── Auto-fill master dropdowns from saved values ───────────────── */
  (function autofillMaster() {
    var selects = [
      { id: 'setvoltage',     val: '%voltage%'         },
      { id: 'microsteps',     val: '%microsteps%'      },
      { id: 'current',        val: '%current%'         },
      { id: 'stall_threshold', val: '%stall_threshold%' },
      { id: 'standstill_mode', val: '%standstill_mode%' },
    ];
    selects.forEach(function(s) {
      var el = document.getElementById(s.id);
      if (!el) return;
      for (var j = 0; j < el.options.length; j++) {
        if (el.options[j].value === s.val) { el.selectedIndex = j; break; }
      }
    });
  })();

  /* ── Master live-stats polling ──────────────────────────────────── */
  function pollMaster(path, elemId, interval) {
    setInterval(function() {
      var x = new XMLHttpRequest();
      x.onreadystatechange = function() {
        if (this.readyState === 4 && this.status === 200)
          document.getElementById(elemId).innerHTML = this.responseText;
      };
      x.open('GET', path, true);
      x.send();
    }, interval);
  }
  pollMaster('/voltage',    'voltage',    390);
  pollMaster('/powergood',  'powergood',  410);
  pollMaster('/status',     'status',     300);
  pollMaster('/position',   'position',   130);
  pollMaster('/stallguard', 'stallguard', 150);

  /* ── Slave live-stats polling ───────────────────────────────────── */
  function pollSlave(path, elemId, interval) {
    setInterval(function() {
      var x = new XMLHttpRequest();
      x.onreadystatechange = function() {
        if (this.readyState === 4 && this.status === 200)
          document.getElementById(elemId).innerHTML = this.responseText;
      };
      x.open('GET', path, true);
      x.send();
    }, interval);
  }
  pollSlave('/slave/voltage',   'slave-voltage',   800);
  pollSlave('/slave/powergood', 'slave-powergood', 820);
  pollSlave('/slave/status',    'slave-status',    780);
  pollSlave('/slave/position',  'slave-position',  760);

  /* ── Slave connection badge ─────────────────────────────────────── */
  setInterval(function() {
    var x = new XMLHttpRequest();
    x.onreadystatechange = function() {
      if (this.readyState !== 4 || this.status !== 200) return;
      var el = document.getElementById('slave-connected');
      el.innerHTML = this.responseText;
      el.className = (this.responseText === 'Connected') ? 'ok' : 'bad';
    };
    x.open('GET', '/slave/connected', true);
    x.send();
  }, 2000);

  /* ── Master velocity slider ─────────────────────────────────────── */
  var masterLastCall = 0;
  var masterThrottle = 100;
  var masterResetEnabled = false;

  function throttledUpdate() {
    var now = Date.now();
    if (now - masterLastCall < masterThrottle) return;
    masterLastCall = now;
    updateSlider();
  }
  function updateSlider() {
    var val = document.getElementById('slider').value;
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/update', true);
    xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    xhr.send('slider=' + val);
  }
  function toggleReset() {
    masterResetEnabled = document.getElementById('toggleReset').checked;
    if (masterResetEnabled) resetSlider();
  }
  function checkReset() { if (masterResetEnabled) resetSlider(); }
  function resetSlider() {
    document.getElementById('slider').value = 0;
    updateSlider();
  }

  /* ── Master position buttons ────────────────────────────────────── */
  function masterPos(n) {
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/update', true);
    xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    xhr.send('positionControl=' + n);
  }
  function but1() { masterPos(1); }
  function but2() { masterPos(2); }
  function but3() { masterPos(3); }
  function but4() { masterPos(4); }

  /* ── Slave velocity slider ──────────────────────────────────────── */
  var slaveLastCall = 0;
  var slaveThrottleMs = 100;
  var slaveResetEnabled = false;

  function slaveThrottledUpdate() {
    var now = Date.now();
    if (now - slaveLastCall < slaveThrottleMs) return;
    slaveLastCall = now;
    slaveUpdateSlider();
  }
  function slaveUpdateSlider() {
    var val = document.getElementById('slave-slider').value;
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/slave/update', true);
    xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    xhr.send('slider=' + val);
  }
  function slaveToggleReset() {
    slaveResetEnabled = document.getElementById('slaveToggleReset').checked;
    if (slaveResetEnabled) slaveResetSlider();
  }
  function slaveCheckReset() { if (slaveResetEnabled) slaveResetSlider(); }
  function slaveResetSlider() {
    document.getElementById('slave-slider').value = 0;
    slaveUpdateSlider();
  }

  /* ── Slave position buttons ─────────────────────────────────────── */
  function slavePos(n) {
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/slave/update', true);
    xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    xhr.send('positionControl=' + n);
  }
  function slaveBut1() { slavePos(1); }
  function slaveBut2() { slavePos(2); }
  function slaveBut3() { slavePos(3); }
  function slaveBut4() { slavePos(4); }
</script>

</body></html>
)rawliteral";
