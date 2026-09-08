/*
 * TSM - Temporal Signal Medium
 * A tape transport for the browser, driven by the reference tools.
 *
 * Copyright (c) 2026 Francesco De Simone
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. A copy is in LICENSE
 * beside this file, and at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Redistributions and derivative works must carry the attribution in NOTICE.
 */

/*
 * What this is, and what it deliberately is not.
 *
 * It plays a TSM the way a cassette deck plays a cassette: the file is decoded
 * once into a signal, and the transport moves along that signal forwards,
 * backwards, and faster. It does not demodulate anything. A TSM records when
 * the signal changed and says nothing about what the changes meant, so a
 * player that claimed to show you "the program" would be inventing a layer the
 * format does not have. What you hear is the tape.
 *
 * Nothing here decodes a TSM either. tsm.js is the published reference tool
 * compiled to WebAssembly, so this file only moves its output around.
 *
 * There is no recording. A player writes nothing.
 */

'use strict';

/* ////////////////////////////////////////////////////////////////////////// */
/* Constants                                                                  */
/* ////////////////////////////////////////////////////////////////////////// */

/* Region modes, from section 2.2 of the specification. */
var TSM_MODES = {
  0: { name: 'SILENCE',      signal: false },
  3: { name: 'INDEXED_DELTA', signal: true },
  4: { name: 'ZERO_SILENCE', signal: false }
};

/* Label kinds, from section 2.5. An unknown kind is shown by number rather
 * than hidden: the format lets the list grow, and a player that swallowed what
 * it did not recognise would make new kinds invisible instead of merely
 * unstyled. */
var TSM_LABEL_KINDS = {
  1: 'name',
  2: 'profile',
  3: 'origin',
  4: 'load'
};

var TSM_LABEL_TAPE = 4294967295;   /* 0xFFFFFFFF: applies to the whole tape */

/* How fast the wind buttons move, as a multiple of play speed. */
var WIND_RATE = 12;

/* Wind is shrill at twelve times speed. It is not silenced, because a deck
 * winding is a sound, but it is held well back. */
var WIND_GAIN = 0.18;

/* The mechanical counter advances this many times per second of tape, chosen
 * so a three-digit counter covers a C-90 without wrapping. */
var COUNTER_PER_SECOND = 1.6;

/* ////////////////////////////////////////////////////////////////////////// */
/* The player                                                                 */
/* ////////////////////////////////////////////////////////////////////////// */

var TSMPlayer = {

  /* ---- wasm and audio ---------------------------------------------------- */
  mod:      null,   /* the Emscripten module, once it has loaded */
  ctx:      null,   /* AudioContext, created on the first user gesture */
  gainNode: null,

  /* ---- the tape ---------------------------------------------------------- */
  info:     null,   /* what tsm_web_describe() said */
  forward:  null,   /* AudioBuffer, the signal as recorded */
  backward: null,   /* the same reversed, built the first time it is needed */
  fileName: '',
  fileSize: 0,
  profile:  null,   /* edge rate per waveform column, in Hz */
  profileMax: 0,    /* the fastest column, for scaling */
  profileCols: 0,

  /* ---- transport --------------------------------------------------------- */
  state:    'empty',  /* empty | stopped | playing | reverse | wind | paused */
  rate:     0,        /* signed multiple of play speed; 0 when not moving */
  position: 0,        /* seconds from the start of the tape */
  source:   null,     /* the AudioBufferSourceNode currently running */
  _startedAt: 0,      /* ctx.currentTime when it started */
  _startPos:  0,      /* position at that moment */

  counterZero: 0,     /* where the mechanical counter was last reset */

  dom: {},

  /* ------------------------------------------------------------------------ */

  /* Find the elements, wire the controls, and start loading the wasm.
   * The AudioContext is NOT created here: browsers require a user gesture, and
   * one made at load time starts suspended and stays that way on some of them.
   * @returns {void} */
  init: function () {
    var self = this;
    var ids = ['file-input', 'drop-zone', 'deck', 'counter', 'time-now',
               'time-total', 'wave', 'status', 'info', 'reel-l', 'reel-r',
               'label-list', 'region-list', 'tape-name'];

    ids.forEach(function (id) {
      self.dom[id] = document.getElementById(id);
    });

    /* Transport buttons carry their command in a data attribute, so the markup
     * decides what exists and this file does not have to know the layout. */
    var buttons = document.querySelectorAll('[data-cmd]');
    Array.prototype.forEach.call(buttons, function (b) {
      b.addEventListener('click', function () {
        self.command(b.getAttribute('data-cmd'));
      });
    });

    if (this.dom['file-input']) {
      this.dom['file-input'].addEventListener('change', function (e) {
        if (e.target.files && e.target.files[0]) {
          self.loadFile(e.target.files[0]);
        }
      });
    }

    this.wireDropZone();
    this.wireKeyboard();

    if (this.dom.wave) {
      this.dom.wave.addEventListener('click', function (e) {
        self.seekFromCanvas(e);
      });
    }

    window.addEventListener('resize', function () { self.drawWave(); });

    this.drawWave();
    this.setStatus('Loading the decoder…');

    createTSM().then(function (m) {
      self.mod = m;
      self.setStatus('Drop a .tsm file on the deck, or use Open.');
      self.updateButtons();
    }).catch(function (err) {
      self.setStatus('The decoder failed to load: ' + err);
    });

    this.tick();
  }, // End init()

  /* Accept a file dropped anywhere on the page.
   * @returns {void} */
  wireDropZone: function () {
    var self = this;
    var zone = this.dom['drop-zone'] || document.body;

    ['dragenter', 'dragover'].forEach(function (name) {
      zone.addEventListener(name, function (e) {
        e.preventDefault();
        zone.classList.add('dragging');
      });
    });

    ['dragleave', 'drop'].forEach(function (name) {
      zone.addEventListener(name, function (e) {
        e.preventDefault();
        if (name === 'dragleave' && e.target !== zone) return;
        zone.classList.remove('dragging');
      });
    });

    zone.addEventListener('drop', function (e) {
      e.preventDefault();
      if (e.dataTransfer.files && e.dataTransfer.files[0]) {
        self.loadFile(e.dataTransfer.files[0]);
      }
    });
  }, // End wireDropZone()

  /* Space plays and stops; the arrows wind. A deck has few enough controls to
   * be worth reaching without the mouse.
   * @returns {void} */
  wireKeyboard: function () {
    var self = this;

    document.addEventListener('keydown', function (e) {
      if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') {
        return;
      }

      var map = {
        ' ':          function () {
                        self.command(self.isMoving() ? 'STOP' : 'PLAY');
                      },
        'ArrowLeft':  function () { self.command('REWIND'); },
        'ArrowRight': function () { self.command('FORWARD'); },
        'ArrowDown':  function () { self.command('STOP'); },
        'r':          function () { self.command('REVERSE'); },
        '0':          function () { self.command('RESET'); }
      };

      if (map[e.key]) {
        e.preventDefault();
        map[e.key]();
      }
    });
  }, // End wireKeyboard()

  /* ////////////////////////////////////////////////////////////////////////// */
  /* Loading                                                                    */
  /* ////////////////////////////////////////////////////////////////////////// */

  /* Read a file, ask the wasm what is on it, and render it to a buffer.
   * @param {File} file - what the user dropped or chose
   * @returns {void} */
  loadFile: function (file) {
    var self = this;
    var reader = new FileReader();

    if (!this.mod) {
      this.setStatus('The decoder is still loading.');
      return;
    }

    this.command('EJECT');
    this.setStatus('Reading ' + file.name + '…');

    reader.onload = function () {
      self.fileName = file.name;
      self.fileSize = file.size;
      self.accept(new Uint8Array(reader.result));
    };

    reader.onerror = function () {
      self.setStatus('Could not read ' + file.name + '.');
    };

    reader.readAsArrayBuffer(file);
  }, // End loadFile()

  /* Describe and render the bytes of a TSM.
   * @param {Uint8Array} bytes - the whole file
   * @returns {void} */
  accept: function (bytes) {
    var info;
    var t0;

    this.ensureContext();

    info = this.describe(bytes);

    if (!info) {
      this.setStatus('That does not look like a TSM v5 file.');
      return;
    }

    if (info.error) {
      this.setStatus(info.error);
      return;
    }

    this.info = info;
    this.setStatus('Rendering the signal…');

    /* Rendered at the context's own rate, so the browser never resamples it
     * and what is heard is what the file says, edge for edge. */
    t0 = performance.now();
    this.forward = this.render(bytes, this.ctx.sampleRate);
    this.backward = null;

    if (!this.forward) {
      this.setStatus('The file described itself but would not render.');
      return;
    }

    this.position = 0;
    this.counterZero = 0;
    this.state = 'stopped';
    this.buildProfile();
    this.showInfo();
    this.drawWave();
    this.updateButtons();

    this.setStatus(this.fileName + ' — ' +
                   this.forward.duration.toFixed(2) + ' s, ' +
                   info.regions.length + ' regions, ' +
                   info.labels.length + ' labels, decoded in ' +
                   Math.round(performance.now() - t0) + ' ms');
  }, // End accept()

  /* Ask the wasm what the file says about itself.
   * @param {Uint8Array} bytes - the whole file
   * @returns {Object|null} the parsed JSON, or null if it could not be read */
  describe: function (bytes) {
    var M = this.mod;
    var p = M._malloc(bytes.length);
    var json = null;
    var s;

    try {
      M.HEAPU8.set(bytes, p);
      s = M.ccall('tsm_web_describe', 'number', ['number', 'number'],
                  [p, bytes.length]);

      if (s) {
        json = M.UTF8ToString(s);
        M.ccall('tsm_web_free', null, ['number'], [s]);
      }
    } finally {
      M._free(p);
    }

    if (!json) return null;

    try {
      return JSON.parse(json);
    } catch (e) {
      return null;
    }
  }, // End describe()

  /* Render the signal into an AudioBuffer.
   * @param {Uint8Array} bytes - the whole file
   * @param {number} rate      - samples per second to render at
   * @returns {AudioBuffer|null} */
  render: function (bytes, rate) {
    var M = this.mod;
    var p = M._malloc(bytes.length);
    var countPtr = M._malloc(4);
    var samplesPtr = 0;
    var buffer = null;
    var count;
    var cb;

    try {
      M.HEAPU8.set(bytes, p);

      samplesPtr = M.ccall(
        'tsm_web_render', 'number',
        ['number', 'number', 'number', 'number', 'number'],
        [p, bytes.length, Math.round(rate), 22000, countPtr]);

      if (!samplesPtr) return null;

      /* HEAP32 is not among the exported views, so the count is read as the
       * four little-endian bytes it is. */
      cb = M.HEAPU8.subarray(countPtr, countPtr + 4);
      count = cb[0] | (cb[1] << 8) | (cb[2] << 16) | (cb[3] << 24);

      if (count <= 0) return null;

      buffer = this.ctx.createBuffer(1, count, Math.round(rate));
      /* HEAPF32 is read here and not before: an allocation may have grown the
       * heap and left an earlier view detached. */
      buffer.getChannelData(0).set(
        M.HEAPF32.subarray(samplesPtr >> 2, (samplesPtr >> 2) + count));
    } finally {
      if (samplesPtr) M.ccall('tsm_web_free', null, ['number'], [samplesPtr]);
      M._free(countPtr);
      M._free(p);
    }

    return buffer;
  }, // End render()

  /* ////////////////////////////////////////////////////////////////////////// */
  /* Transport                                                                  */
  /* ////////////////////////////////////////////////////////////////////////// */

  /* Create the AudioContext, or wake one the browser suspended.
   * @returns {void} */
  ensureContext: function () {
    if (!this.ctx) {
      var Ctor = window.AudioContext || window.webkitAudioContext;
      this.ctx = new Ctor();
      this.gainNode = this.ctx.createGain();
      this.gainNode.connect(this.ctx.destination);
    }

    if (this.ctx.state === 'suspended') {
      this.ctx.resume();
    }
  }, // End ensureContext()

  /* @returns {boolean} whether the tape is moving under any command */
  isMoving: function () {
    return this.rate !== 0;
  }, // End isMoving()

  /* Act on a transport button.
   * @param {string} cmd - PLAY, REVERSE, REWIND, FORWARD, STOP, RESET, EJECT
   * @returns {void} */
  command: function (cmd) {
    if (cmd === 'EJECT') {
      this.halt();
      this.info = null;
      this.forward = null;
      this.backward = null;
      this.profile = null;
      this.position = 0;
      this.state = 'empty';
      this.fileName = '';
      this.showInfo();
      this.drawWave();
      this.updateButtons();
      this.setStatus('Drop a .tsm file on the deck, or use Open.');
      return;
    }

    if (cmd === 'RESET') {
      this.counterZero = this.position;
      return;
    }

    if (!this.forward) return;

    this.ensureContext();

    switch (cmd) {
      case 'PLAY':    this.run(1, 'playing');           break;
      case 'REVERSE': this.run(-1, 'reverse');          break;
      case 'REWIND':  this.run(-WIND_RATE, 'wind');     break;
      case 'FORWARD': this.run(WIND_RATE, 'wind');      break;
      case 'STOP':    this.halt(); this.state = 'stopped'; break;
      default: return;
    }

    this.updateButtons();
  }, // End command()

  /* Start the tape moving at a signed multiple of play speed.
   * @param {number} rate - negative runs backwards
   * @param {string} state - what to call it
   * @returns {void} */
  run: function (rate, state) {
    var self = this;
    var buffer;
    var offset;
    var src;

    this.halt();

    /* At the end of the tape a forward command has nothing to play, and at the
     * start a backward one does not either. Silently doing nothing would look
     * like a broken button, so the tape is wound to the other end first - the
     * same thing a real deck's end-stop leaves you to do by hand. */
    if (rate > 0 && this.position >= this.forward.duration - 0.001) {
      this.position = 0;
    }
    if (rate < 0 && this.position <= 0.001) {
      this.position = this.forward.duration;
    }

    if (rate < 0) {
      buffer = this.reverseBuffer();
      offset = this.forward.duration - this.position;
    } else {
      buffer = this.forward;
      offset = this.position;
    }

    src = this.ctx.createBufferSource();
    src.buffer = buffer;
    src.playbackRate.value = Math.abs(rate);
    src.connect(this.gainNode);

    this.gainNode.gain.value =
      (Math.abs(rate) > 1) ? WIND_GAIN : 1.0;

    /* This fires only when the buffer runs out, because halt() detaches the
     * handler before stopping a source on purpose. */
    src.onended = function () {
      self.halt();
      self.state = 'stopped';
      self.position = (rate > 0) ? self.forward.duration : 0;
      self.updateButtons();
    };

    src.start(0, Math.max(0, Math.min(offset, buffer.duration - 0.0001)));

    this.source = src;
    this.rate = rate;
    this.state = state;
    this._startedAt = this.ctx.currentTime;
    this._startPos = this.position;
  }, // End run()

  /* Stop whatever is playing, keeping the position where it got to.
   * @returns {void} */
  halt: function () {
    if (this.source) {
      this.position = this.currentPosition();

      /* Detached first: onended arrives after this function has returned, and
       * a flag cleared synchronously would already be false by then, so every
       * deliberate stop would look like the tape running out. */
      this.source.onended = null;
      try { this.source.stop(); } catch (e) { /* already finished */ }
      this.source.disconnect();
      this.source = null;
    }

    this.rate = 0;
  }, // End halt()

  /* Where the tape has got to, computed from the audio clock rather than from
   * a timer, so it cannot drift away from what is being heard.
   * @returns {number} seconds from the start of the tape */
  currentPosition: function () {
    var p;

    if (!this.forward) return 0;
    if (!this.source) return this.position;

    p = this._startPos + (this.ctx.currentTime - this._startedAt) * this.rate;

    return Math.max(0, Math.min(p, this.forward.duration));
  }, // End currentPosition()

  /* The signal backwards. Built once, and only if the user ever runs the tape
   * that way, because it costs as much memory again as the tape itself.
   * @returns {AudioBuffer} */
  reverseBuffer: function () {
    var src;
    var dst;
    var n;
    var i;

    if (this.backward) return this.backward;

    n = this.forward.length;
    this.backward = this.ctx.createBuffer(1, n, this.forward.sampleRate);
    src = this.forward.getChannelData(0);
    dst = this.backward.getChannelData(0);

    for (i = 0; i < n; i++) {
      dst[i] = src[n - 1 - i];
    }

    return this.backward;
  }, // End reverseBuffer()

  /* Wind to a point picked on the waveform.
   * @param {MouseEvent} e - the click
   * @returns {void} */
  seekFromCanvas: function (e) {
    var rect;
    var was;

    if (!this.forward) return;

    rect = this.dom.wave.getBoundingClientRect();
    was = this.state;

    this.halt();
    this.position = this.forward.duration *
                    Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));

    if (was === 'playing' || was === 'reverse') {
      this.run(was === 'playing' ? 1 : -1, was);
    } else {
      this.state = 'stopped';
    }

    this.updateButtons();
  }, // End seekFromCanvas()

  /* ////////////////////////////////////////////////////////////////////////// */
  /* Display                                                                    */
  /* ////////////////////////////////////////////////////////////////////////// */

  /* The animation loop. It only touches the DOM when something changed, so an
   * idle deck costs nothing.
   * @returns {void} */
  tick: function () {
    var self = this;
    var pos = this.currentPosition();

    if (this.isMoving()) {
      this.position = pos;
    }

    this.updateTime(pos);
    this.updateReels();
    this.drawPlayhead();

    requestAnimationFrame(function () { self.tick(); });
  }, // End tick()

  /* @param {number} pos - seconds into the tape
   * @returns {void} */
  updateTime: function (pos) {
    var total = this.forward ? this.forward.duration : 0;
    var count;

    if (this.dom['time-now']) {
      this.dom['time-now'].textContent = this.clock(pos);
    }

    if (this.dom['time-total']) {
      this.dom['time-total'].textContent = this.clock(total);
    }

    if (this.dom.counter) {
      count = Math.floor((pos - this.counterZero) * COUNTER_PER_SECOND);
      count = ((count % 1000) + 1000) % 1000;
      this.dom.counter.textContent = String(count).padStart(3, '0');
    }
  }, // End updateTime()

  /* @param {number} s - seconds
   * @returns {string} m:ss */
  clock: function (s) {
    var m = Math.floor(s / 60);
    var r = Math.floor(s % 60);
    return m + ':' + String(r).padStart(2, '0');
  }, // End clock()

  /* Turn the reels at a speed and direction that matches the transport.
   * @returns {void} */
  updateReels: function () {
    var spin = this.rate * 40;   /* degrees per second at play speed */
    var now;
    var dt;

    if (!this.dom['reel-l']) return;

    now = performance.now() / 1000;

    /* Frames stop while the tab is hidden, so the gap on returning can be
     * minutes. Clamped to one frame: the reels are not a clock, and the
     * position they illustrate is read from the audio clock anyway. */
    dt = Math.min(now - (this._reelTime || now), 0.05);
    this._reelTime = now;

    this._reelAngle = ((this._reelAngle || 0) + spin * dt) % 360;

    this.dom['reel-l'].style.transform =
      'rotate(' + this._reelAngle.toFixed(1) + 'deg)';

    if (this.dom['reel-r']) {
      this.dom['reel-r'].style.transform =
        'rotate(' + this._reelAngle.toFixed(1) + 'deg)';
    }
  }, // End updateReels()

  /* Mark which buttons are live and which one is engaged.
   * @returns {void} */
  updateButtons: function () {
    var self = this;
    var loaded = !!this.forward;
    var engaged = {
      playing: 'PLAY',
      reverse: 'REVERSE',
      stopped: 'STOP'
    }[this.state];

    if (this.state === 'wind') {
      engaged = (this.rate < 0) ? 'REWIND' : 'FORWARD';
    }

    Array.prototype.forEach.call(
      document.querySelectorAll('[data-cmd]'),
      function (b) {
        var cmd = b.getAttribute('data-cmd');
        var live = loaded || cmd === 'EJECT' || cmd === 'RESET';

        b.disabled = !live || (!self.mod && cmd !== 'RESET');
        b.classList.toggle('engaged', cmd === engaged);
      });

    if (this.dom.deck) {
      this.dom.deck.classList.toggle('loaded', loaded);
      this.dom.deck.setAttribute('data-state', this.state);
    }
  }, // End updateButtons()

  /* @param {string} text - what the deck is doing
   * @returns {void} */
  setStatus: function (text) {
    if (this.dom.status) this.dom.status.textContent = text;
  }, // End setStatus()

  /* ////////////////////////////////////////////////////////////////////////// */
  /* What the file says                                                         */
  /* ////////////////////////////////////////////////////////////////////////// */

  /* Fill the region table and the label list.
   * @returns {void} */
  showInfo: function () {
    var info = this.info;
    var tickNs;
    var rows;
    var self = this;

    if (this.dom['tape-name']) {
      this.dom['tape-name'].textContent = this.fileName || '—';
    }

    if (!info) {
      if (this.dom.info) this.dom.info.textContent = '';
      if (this.dom['region-list']) this.dom['region-list'].innerHTML = '';
      if (this.dom['label-list']) this.dom['label-list'].innerHTML = '';
      return;
    }

    tickNs = info.tick_ns;

    if (this.dom.info) {
      this.dom.info.innerHTML = '';
      [['Format',   'TSM v' + info.version],
       ['Size',     this.bytes(this.fileSize)],
       ['Duration', (info.total_ticks * tickNs / 1e9).toFixed(2) + ' s'],
       ['Tick',     tickNs + ' ns'],
       ['Regions',  String(info.regions.length)],
       ['Recorded', info.source_rate
                      ? info.source_samples.toLocaleString() + ' samples at ' +
                        info.source_rate.toLocaleString() + ' Hz'
                      : 'not stated']
      ].forEach(function (pair) {
        var dt = document.createElement('dt');
        var dd = document.createElement('dd');
        dt.textContent = pair[0];
        dd.textContent = pair[1];
        self.dom.info.appendChild(dt);
        self.dom.info.appendChild(dd);
      });
    }

    /* ---- regions ---------------------------------------------------------- */
    if (this.dom['region-list']) {
      rows = info.regions.map(function (r, i) {
        var mode = TSM_MODES[r.mode] || { name: String(r.mode), signal: true };
        var a = r.start_ticks * r.tick_ns / 1e9;
        var b = (r.start_ticks + r.duration_ticks) * r.tick_ns / 1e9;
        var names = self.labelsFor(i, 1);
        var profiles = self.labelsFor(i, 2);

        return '<tr class="' + (mode.signal ? 'signal' : 'quiet') + '">' +
               '<td class="n">' + i + '</td>' +
               '<td>' + self.clock(a) + '–' + self.clock(b) + '</td>' +
               '<td>' + mode.name + '</td>' +
               '<td class="n">' + (r.data_size
                                     ? r.data_size.toLocaleString() + ' B'
                                     : '—') + '</td>' +
               '<td class="profile">' +
                 self.escape(profiles.join(', ') || '') + '</td>' +
               '<td>' + self.escape(names.join(', ')) + '</td>' +
               '</tr>';
      });

      this.dom['region-list'].innerHTML = rows.join('');
    }

    /* ---- labels ----------------------------------------------------------- */
    if (this.dom['label-list']) {
      if (!info.labels.length) {
        this.dom['label-list'].innerHTML =
          '<li class="none">This tape carries no labels. ' +
          'The section is optional, and a file written before it existed ' +
          'has none.</li>';
      } else {
        this.dom['label-list'].innerHTML = info.labels.map(function (l) {
          var kind = TSM_LABEL_KINDS[l.kind] || ('kind ' + l.kind);
          var where = (l.region === TSM_LABEL_TAPE)
                        ? 'whole tape'
                        : 'region ' + l.region;

          return '<li><span class="kind k-' + kind + '">' + kind + '</span>' +
                 '<span class="where">' + where + '</span>' +
                 '<span class="text">' + self.escape(l.text) + '</span></li>';
        }).join('');
      }
    }
  }, // End showInfo()

  /* The text of every label of one kind attached to a region.
   * @param {number} region - its index
   * @param {number} kind   - a TSM_LABEL_KINDS key
   * @returns {Array<string>} */
  labelsFor: function (region, kind) {
    if (!this.info) return [];

    return this.info.labels.filter(function (l) {
      return l.region === region && l.kind === kind;
    }).map(function (l) { return l.text; });
  }, // End labelsFor()

  /* @param {number} n - a byte count
   * @returns {string} it, in units a person reads */
  bytes: function (n) {
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    return (n / (1024 * 1024)).toFixed(1) + ' MB';
  }, // End bytes()

  /* Label text comes out of a file somebody else wrote, so it is escaped
   * rather than trusted.
   * @param {string} s - the text
   * @returns {string} it, safe to place in HTML */
  escape: function (s) {
    return String(s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;',
               '"': '&quot;', "'": '&#39;' }[c];
    });
  }, // End escape()

  /* ////////////////////////////////////////////////////////////////////////// */
  /* The waveform                                                               */
  /* ////////////////////////////////////////////////////////////////////////// */

  /* Measure how often the signal crosses zero, one figure per column.
   *
   * A peak envelope would be honest and useless: a carrier tone fills every
   * column from -peak to +peak, so the whole tape draws as one solid block.
   * What distinguishes a pilot tone from data, and a KCS block from a ZX
   * pulse block, is how FAST the signal changes - which is the only thing a
   * TSM stores. So that is what is drawn.
   *
   * Done once per tape: five million samples is more than a frame can afford.
   * @returns {void} */
  buildProfile: function () {
    var data;
    var cols = 1400;
    var per;
    var i;
    var j;
    var from;
    var to;
    var crossings;
    var was;
    var now;
    var seconds;
    var hz;

    if (!this.forward) { this.profile = null; return; }

    data = this.forward.getChannelData(0);
    per = data.length / cols;
    seconds = per / this.forward.sampleRate;
    this.profile = new Float32Array(cols);
    this.profileCols = cols;
    this.profileMax = 0;

    for (i = 0; i < cols; i++) {
      from = Math.floor(i * per);
      to = Math.min(data.length, Math.floor((i + 1) * per));
      crossings = 0;
      was = data[from] >= 0;

      for (j = from; j < to; j++) {
        now = data[j] >= 0;
        if (now !== was) { crossings++; was = now; }
      }

      /* Two crossings to a cycle. */
      hz = (crossings / 2) / seconds;
      this.profile[i] = hz;
      if (hz > this.profileMax) this.profileMax = hz;
    }
  }, // End buildProfile()

  /* Draw the tape: a band per region, the signal over it.
   * @returns {void} */
  drawWave: function () {
    var c = this.dom.wave;
    var g;
    var w;
    var h;
    var dpr = window.devicePixelRatio || 1;
    var self = this;
    var total;

    if (!c) return;

    w = c.clientWidth;
    h = c.clientHeight;
    c.width = Math.round(w * dpr);
    c.height = Math.round(h * dpr);
    g = c.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);

    g.clearRect(0, 0, w, h);

    g.fillStyle = this.cssVar('--wave-bg', '#12100e');
    g.fillRect(0, 0, w, h);

    if (!this.forward || !this.profile) {
      g.fillStyle = this.cssVar('--wave-idle', '#3a352e');
      g.fillRect(0, h - 3.5, w, 1);
      return;
    }

    total = this.forward.duration;

    /* Region bands first: where the tape is quiet is as much a part of what is
     * on it as where it is not. */
    this.info.regions.forEach(function (r) {
      var mode = TSM_MODES[r.mode] || { signal: true };
      var a = (r.start_ticks * r.tick_ns / 1e9) / total * w;
      var b = ((r.start_ticks + r.duration_ticks) * r.tick_ns / 1e9) / total * w;

      g.fillStyle = mode.signal
                      ? self.cssVar('--band-signal', '#1d2b24')
                      : self.cssVar('--band-quiet', '#171514');
      g.fillRect(a, 0, Math.max(1, b - a), h);
    });

    /* The edge rate, as an area from the bottom: where the tape is quiet it
     * falls to nothing, and a change of protocol is a step in the outline. */
    var top = this.profileMax || 1;
    var colW = w / this.profileCols;

    g.fillStyle = this.cssVar('--wave-ink', '#7fd1a4');
    g.beginPath();
    g.moveTo(0, h);

    for (var i = 0; i < this.profileCols; i++) {
      var x = i * colW;
      var y = h - (this.profile[i] / top) * (h - 6) - 3;
      g.lineTo(x, y);
      g.lineTo(x + colW, y);
    }

    g.lineTo(w, h);
    g.closePath();
    g.globalAlpha = 0.85;
    g.fill();
    g.globalAlpha = 1;

    /* The scale, so the shape is a measurement and not a decoration. One line
     * at the top, where the area never reaches. */
    g.fillStyle = this.cssVar('--ink-dim', '#9a9186');
    g.font = '10px ui-monospace, Menlo, Consolas, monospace';
    g.fillText('edge rate \u00b7 peak ' + Math.round(top) + ' Hz', 6, 13);

    this._waveW = w;
    this._waveH = h;
  }, // End drawWave()

  /* The playhead moves every frame, so it is drawn on top of the canvas as an
   * element rather than by redrawing the waveform.
   * @returns {void} */
  drawPlayhead: function () {
    var head = document.getElementById('playhead');
    var total;

    if (!head) return;

    if (!this.forward) {
      head.style.display = 'none';
      return;
    }

    total = this.forward.duration;
    head.style.display = 'block';
    head.style.left = (this.currentPosition() / total * 100) + '%';
  }, // End drawPlayhead()

  /* @param {string} name - a CSS custom property
   * @param {string} fallback - what to use if the page does not define it
   * @returns {string} its value */
  cssVar: function (name, fallback) {
    var v = getComputedStyle(document.documentElement)
              .getPropertyValue(name).trim();
    return v || fallback;
  } // End cssVar()

};

window.TSMPlayer = TSMPlayer;

document.addEventListener('DOMContentLoaded', function () {
  TSMPlayer.init();
});
