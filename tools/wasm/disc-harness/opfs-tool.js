// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Disc harness: OPFS chores in a worker (sync access handles exist only in dedicated workers).
//   import {url, name, limit?}  streams the URL (or its first `limit` bytes) into OPFS and times it
//   makeSynthetic {name, size}, truncate {name, size}   for the changed-file tests
//   writeSpeed {mb}      times plain sync access handle writes
//   remove {name}, list, estimate

'use strict';

async function fileHandle(name, create = false) {
	const root = await navigator.storage.getDirectory();
	return root.getFileHandle(name, { create });
}

const commands = {
	async import(msg) {
		const before = await navigator.storage.estimate();
		const handle = await fileHandle(msg.name, true);
		const access = await handle.createSyncAccessHandle();
		const t0 = performance.now();
		let written = 0;
		let lastReport = t0;
		try {
			access.truncate(0);
			const res = await fetch(msg.url, { cache: 'no-store' });
			if (!res.ok)
				throw new Error(`HTTP ${res.status}`);
			const reader = res.body.getReader();
			for (;;) {
				let { done, value } = await reader.read();
				if (done)
					break;
				if (msg.limit && written + value.byteLength > msg.limit) {
					value = value.subarray(0, msg.limit - written); // a partial copy, for read tests
					done = true;
				}
				// Chrome 152 returns (u32)-8 (FILE_ERROR_NO_SPACE) here instead of throwing once a file
				// would pass 2000 MiB, and the file stays at that size: check every return value.
				let off = 0;
				while (off < value.byteLength) {
					const n = access.write(value.subarray(off), { at: written + off });
					if (!(n > 0 && n <= value.byteLength - off))
						throw new Error(`OPFS refused to write at ${written + off} (write returned ${n}, file size ${access.getSize()})`);
					off += n;
				}
				written += value.byteLength;
				if (done) {
					reader.cancel().catch(() => {});
					break;
				}
				const now = performance.now();
				if (now - lastReport > 1000) {
					lastReport = now;
					self.postMessage({ progress: written, ms: now - t0 });
				}
			}
			access.flush();
			if (access.getSize() !== written)
				throw new Error(`OPFS file is ${access.getSize()} bytes after writing ${written}`);
		} catch (err) {
			access.close();
			const root = await navigator.storage.getDirectory();
			await root.removeEntry(msg.name).catch(() => {});
			throw err;
		}
		access.close();
		const ms = performance.now() - t0;
		const after = await navigator.storage.estimate();
		return { bytes: written, ms: +ms.toFixed(0), mbPerS: +(written / 1048576 / (ms / 1000)).toFixed(0),
			quota: after.quota, usageBefore: before.usage, usageAfter: after.usage };
	},

	// Writes `mb` MiB of zeros through a sync access handle (16 MiB writes) and removes the file.
	async writeSpeed(msg) {
		const name = 'disc-harness-speed.bin';
		const access = await (await fileHandle(name, true)).createSyncAccessHandle();
		const chunk = new Uint8Array(16 * 1024 * 1024);
		const t0 = performance.now();
		let written = 0;
		let failedAt = null;
		for (let k = 0; k < msg.mb / 16; k++) {
			const n = access.write(chunk, { at: written });
			if (n !== chunk.length) {
				failedAt = { at: written, returned: n };
				break;
			}
			written += n;
		}
		access.flush();
		const ms = performance.now() - t0;
		access.close();
		await (await navigator.storage.getDirectory()).removeEntry(name);
		return { bytes: written, seconds: +(ms / 1000).toFixed(2), mbPerS: +(written / 1048576 / (ms / 1000)).toFixed(0), failedAt };
	},

	// A small file of synthetic bytes (no disc data), for the changed-file tests.
	async makeSynthetic(msg) {
		const access = await (await fileHandle(msg.name, true)).createSyncAccessHandle();
		access.truncate(0);
		const data = new Uint8Array(msg.size);
		for (let k = 0; k < data.length; k++)
			data[k] = (k * 7) & 255;
		access.write(data, { at: 0 });
		access.flush();
		access.close();
		return msg.size;
	},

	async truncate(msg) {
		const access = await (await fileHandle(msg.name)).createSyncAccessHandle();
		access.truncate(msg.size);
		access.flush();
		access.close();
		return msg.size;
	},

	async remove(msg) {
		const root = await navigator.storage.getDirectory();
		await root.removeEntry(msg.name);
		return true;
	},

	async list() {
		const root = await navigator.storage.getDirectory();
		const out = [];
		for await (const [name, h] of root.entries())
			out.push({ name, kind: h.kind, size: h.kind === 'file' ? (await h.getFile()).size : 0 });
		return out;
	},

	async estimate() {
		const e = await navigator.storage.estimate();
		return { quota: e.quota, usage: e.usage, persisted: await navigator.storage.persisted?.() };
	},
};

self.onmessage = async (event) => {
	const msg = event.data;
	try {
		self.postMessage({ id: msg.id, ok: true, value: await commands[msg.cmd](msg) });
	} catch (err) {
		self.postMessage({ id: msg.id, ok: false, error: String(err?.message || err) });
	}
};
