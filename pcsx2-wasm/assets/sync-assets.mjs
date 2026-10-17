// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Copies the self-hosted fonts and licences from the pinned packages (see package.json and
// pnpm-lock.yaml) into ../web, and builds ../web/icons.svg: the Phosphor icons listed below
// as <symbol>s (paths copied verbatim) followed by the in-house symbols in icons-local.svg.
// Run after `pnpm install`:  pnpm run sync
// Only Node built-ins, so the page itself keeps no build step.

import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const modules = join(here, 'node_modules');
const web = join(here, '..', 'web');

// [source inside node_modules, destination inside web/]
const FILES = [
	['@fontsource-variable/archivo/files/archivo-latin-wdth-normal.woff2', 'fonts/archivo.woff2'],
	['@fontsource-variable/archivo/LICENSE', 'fonts/Archivo-OFL.txt'],
	['@fontsource-variable/chivo-mono/files/chivo-mono-latin-wght-normal.woff2', 'fonts/chivo-mono.woff2'],
	['@fontsource-variable/chivo-mono/LICENSE', 'fonts/ChivoMono-OFL.txt'],
	['@phosphor-icons/core/LICENSE', 'icons/Phosphor-LICENSE.txt'],
];

// Phosphor Regular icons used by the page; each becomes <symbol id="i-NAME">.
const ICONS = [
	'play', 'stop', 'memory', 'caret-up', 'folder-open', 'trash', 'checkerboard', 'keyboard',
	'terminal-window', 'corners-out', 'corners-in', 'copy', 'x', 'arrow-clockwise', 'arrow-up-right',
	'arrow-line-down', 'arrow-up', 'arrow-down', 'arrow-left', 'arrow-right', 'disc',
	'speaker-high', 'speaker-low', 'speaker-x', 'speaker-slash',
];

for (const [from, to] of FILES) {
	const dest = join(web, to);
	mkdirSync(dirname(dest), { recursive: true });
	copyFileSync(join(modules, from), dest);
	console.log(`${to}  <-  ${from}`);
}

const phosphor = join(modules, '@phosphor-icons/core');
const { version } = JSON.parse(readFileSync(join(phosphor, 'package.json'), 'utf8'));
const lines = [
	'<svg xmlns="http://www.w3.org/2000/svg">',
	`<!-- Phosphor Icons core ${version}, Regular weight (MIT, see icons/Phosphor-LICENSE.txt). Paths copied verbatim. -->`,
];
for (const name of ICONS) {
	const svg = readFileSync(join(phosphor, 'assets/regular', `${name}.svg`), 'utf8');
	const inner = svg.match(/^<svg[^>]*>([\s\S]*)<\/svg>\s*$/);
	if (!inner)
		throw new Error(`Unexpected markup in Phosphor icon "${name}"`);
	lines.push(`<symbol id="i-${name}" viewBox="0 0 256 256" fill="currentColor">${inner[1]}</symbol>`);
}
const local = readFileSync(join(here, 'icons-local.svg'), 'utf8');
writeFileSync(join(web, 'icons.svg'), `${lines.join('\n')}\n${local}</svg>\n`);
console.log(`icons.svg  <-  ${ICONS.length} Phosphor icons + icons-local.svg`);
