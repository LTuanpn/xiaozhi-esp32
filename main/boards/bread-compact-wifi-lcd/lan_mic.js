/* PCM mono 16 kHz, 960 samples per 60 ms frame. */
class LanMic extends AudioWorkletProcessor {
  constructor() {
    super();
    this.step = 16000 / sampleRate;
    this.phase = 0;
    this.sum = 0;
    this.count = 0;
    this.frame = new Int16Array(960);
    this.used = 0;
    this.finished = false;
    this.port.onmessage = ({data}) => {
      if (data !== "flush" || this.finished) return;
      this.finished = true;
      if (this.used) this.sendFrame();
      this.port.postMessage("flushed");
    };
  }

  sendFrame() {
    this.port.postMessage(this.frame.buffer, [this.frame.buffer]);
    this.frame = new Int16Array(960);
    this.used = 0;
  }

  process(inputs) {
    if (this.finished) return false;
    for (const sample of inputs[0]?.[0] ?? []) {
      this.sum += sample;
      this.count++;
      this.phase += this.step;
      if (this.phase < 1) continue;
      this.phase -= 1;
      this.frame[this.used++] = Math.round(Math.max(-1, Math.min(1, this.sum / this.count)) * 32767);
      this.sum = 0;
      this.count = 0;
      if (this.used === this.frame.length) this.sendFrame();
    }
    return true;
  }
}
registerProcessor("lan-mic", LanMic);
