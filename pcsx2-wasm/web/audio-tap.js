// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Test recording for the page's sound (UI-SPEC.md §11, the ?audiotap=SECONDS switch; app.js connectAudioTap()).
// An AudioWorkletProcessor between the emulator's worklet node and the page's gain: it passes its input through
// unchanged and posts it to the page in 100 ms blocks, so a test can compare what the PS2 played with a reference
// (tools/wasm/audio/wavcheck.py) while the output itself stays muted. The page loads it only with the switch.

const BLOCK_FRAMES = 4800;

class TabStationTooTap extends AudioWorkletProcessor {
	constructor() {
		super();
		this.left = new Float32Array(BLOCK_FRAMES);
		this.right = new Float32Array(BLOCK_FRAMES);
		this.count = 0;
		this.at = 0; // context frame of the block's first frame
	}

	process(inputs, outputs) {
		const input = inputs[0] || [];
		const output = outputs[0];
		const frames = output[0].length;
		const l = input[0];
		const r = input[1] || input[0];
		for (let ch = 0; ch < output.length; ch++) {
			const src = input[ch] || (ch === 1 ? r : null);
			if (src)
				output[ch].set(src);
			else
				output[ch].fill(0);
		}

		// An input with no channels (nothing connected yet) records as silence.
		for (let i = 0; i < frames; i++) {
			if (this.count === 0)
				this.at = currentFrame + i;
			this.left[this.count] = l ? l[i] : 0;
			this.right[this.count] = r ? r[i] : 0;
			if (++this.count === BLOCK_FRAMES) {
				this.port.postMessage({ at: this.at, l: this.left, r: this.right }, [this.left.buffer, this.right.buffer]);
				this.left = new Float32Array(BLOCK_FRAMES);
				this.right = new Float32Array(BLOCK_FRAMES);
				this.count = 0;
			}
		}
		return true;
	}
}

registerProcessor('tabstationtoo-tap', TabStationTooTap);
