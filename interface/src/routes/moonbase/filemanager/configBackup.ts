export const CONFIG_BACKUP_SCHEMA = 'moonlight-config-backup';
export const CONFIG_BACKUP_VERSION = 1;
export const CONFIG_BACKUP_FULL_VERSION = 1;
export const CONFIG_BACKUP_MANIFEST = 'manifest.json';
// ponytail: store-only ZIP keeps the bundled UI dependency-free; add deflate only if backup sizes justify it.
export const CONFIG_BACKUP_MAX_FILES = 512;
export const CONFIG_BACKUP_MAX_FILE_BYTES = 256 * 1024;
export const CONFIG_BACKUP_MAX_BYTES = 8 * 1024 * 1024;
export const CONFIG_BACKUP_MAX_ARCHIVE_BYTES = CONFIG_BACKUP_MAX_BYTES + 512 * 1024;
export const CONFIG_BACKUP_MAX_PATH_BYTES = 255;
// PsychicHttp accepts at most 16 KiB request bodies; keep room for its JSON envelope.
export const CONFIG_BACKUP_MAX_REQUEST_BYTES = 15 * 1024;
export const CONFIG_BACKUP_WIFI_SETTINGS_PATH = '/.config/wifiSettings.json';
export const CONFIG_BACKUP_ETHERNET_SETTINGS_PATH = '/.config/ethernetSettings.json';
export const CONFIG_BACKUP_MQTT_SETTINGS_PATH = '/.config/mqttSettings.json';
export const CONFIG_BACKUP_AP_SETTINGS_PATH = '/.config/apSettings.json';
export const CONFIG_BACKUP_REQUIRED_PATHS = [
	CONFIG_BACKUP_WIFI_SETTINGS_PATH,
	CONFIG_BACKUP_AP_SETTINGS_PATH
] as const;

const frameworkSettings = [
	'wifiSettings',
	'apSettings',
	'securitySettings',
	'ntpSettings',
	'mqttSettings',
	'ethernetSettings'
];
const moduleSettings = [
	'tasks',
	'inputoutput',
	'effects',
	'devices',
	'drivers',
	'lightscontrol',
	'channels',
	'moonlightinfo',
	'livescripts'
];
const productLayouts = [
	'L_Vest95_01_Left.sc',
	'L_Vest95_02_Back.sc',
	'L_Vest95_03_Right.sc',
	'L_Vest95_Provisional.sc',
	'L_WhiteVest95.sc',
	'L_WhiteVest95_3D.sc'
];

export const CONFIG_BACKUP_CANDIDATE_PATHS = [
	...frameworkSettings.map((name) => `/.config/${name}.json`),
	...moduleSettings.map((name) => `/.config/${name}.json`),
	...Array.from(
		{ length: 20 },
		(_, index) => `/.config/presets/preset${String(index + 1).padStart(2, '0')}.json`
	),
	...productLayouts.map((name) => `/${name}`)
];

export type FilesStateLike = {
	name: string;
	path: string;
	isFile: boolean;
	size?: number;
	files?: FilesStateLike[];
};

export type BackupFile = { path: string; bytes: Uint8Array };
export type BackupManifestEntry = { path: string; size: number; sha256: string };
export type BackupManifest = {
	schema: typeof CONFIG_BACKUP_SCHEMA;
	version: typeof CONFIG_BACKUP_VERSION;
	fullBackupVersion: typeof CONFIG_BACKUP_FULL_VERSION;
	createdAt: string;
	files: BackupManifestEntry[];
};
export type ParsedBackup = { manifest: BackupManifest; files: BackupFile[] };

function scriptPath(value: string): string | null {
	if (!/\.sc$/i.test(value)) return null;
	let path: string;
	if (value.startsWith('/')) path = value;
	else if (value.startsWith('livescripts/')) path = `/${value}`;
	else if (!value.includes('/')) path = `/${value}`;
	else path = `/livescripts/${value}`;
	return isManagedBackupPath(path) ? normalizeBackupPath(path) : null;
}

export function discoverBackupScriptPaths(files: BackupFile[]): string[] {
	const paths = new Set<string>();
	const visit = (value: unknown) => {
		if (typeof value === 'string') {
			const path = scriptPath(value);
			if (path) paths.add(path);
		} else if (Array.isArray(value)) {
			for (const item of value) visit(item);
		} else if (value && typeof value === 'object') {
			for (const item of Object.values(value)) visit(item);
		}
	};
	for (const file of files) {
		if (!file.path.endsWith('.json')) continue;
		try {
			visit(JSON.parse(decodeBackupText(file.bytes, file.path)));
		} catch {
			// Malformed JSON is reported later by the archive text validation.
		}
	}
	return [...paths].sort();
}

export function getBackupProbePaths(extraPaths: string[] = []): string[] {
	const paths = new Set(CONFIG_BACKUP_CANDIDATE_PATHS);
	for (const path of extraPaths) {
		if (isManagedBackupPath(path)) paths.add(normalizeBackupPath(path));
	}
	return [...paths];
}

const protectedSegments = new Set([
	'recovery',
	'golden',
	'temp',
	'.config-recovery',
	'.config-golden',
	'.config-golden-stage',
	'.config-golden-rollback',
	'.config-restore-stage',
	'.config-restore-rollback',
	'.livescripts-restore-stage',
	'.livescripts-restore-rollback'
]);
const transientName = /(?:\.tmp|\.part|\.stage|\.rollback|\.bak)$/i;

export function normalizeBackupPath(path: string): string {
	if (
		typeof path !== 'string' ||
		!path.startsWith('/') ||
		path.includes('\\') ||
		path.includes('\0')
	) {
		throw new Error('Backup path must be an absolute POSIX path');
	}
	const segments = path.split('/').slice(1);
	if (
		!segments.length ||
		segments.some((segment) => !segment || segment === '.' || segment === '..')
	) {
		throw new Error(`Invalid backup path: ${path}`);
	}
	if (
		segments.some(
			(segment) => protectedSegments.has(segment.toLowerCase()) || transientName.test(segment)
		)
	) {
		throw new Error(`Protected or transient backup path: ${path}`);
	}
	if (segments.some((segment) => /[\u0000-\u001f\u007f]/.test(segment))) {
		throw new Error(`Invalid backup path: ${path}`);
	}
	const normalized = `/${segments.join('/')}`;
	if (new TextEncoder().encode(normalized).length > CONFIG_BACKUP_MAX_PATH_BYTES) {
		throw new Error(`Backup path is too long: ${path}`);
	}
	return normalized;
}

export function isManagedBackupPath(path: string): boolean {
	try {
		const normalized = normalizeBackupPath(path);
		const segments = normalized.slice(1).split('/');
		return (
			segments[0] === '.config' ||
			segments[0] === 'livescripts' ||
			(segments.length === 1 && /\.sc$/i.test(segments[0]))
		);
	} catch {
		return false;
	}
}

export function collectBackupFiles(state: FilesStateLike): string[] {
	const result: string[] = [];
	const visit = (items: FilesStateLike[]) => {
		for (const item of items) {
			if (item.isFile) {
				if (isManagedBackupPath(item.path)) result.push(normalizeBackupPath(item.path));
			} else if (item.files) visit(item.files);
		}
	};
	visit(state.files ?? []);
	return [...new Set(result)].sort();
}

export function collectBackupFileSizes(state: FilesStateLike): Map<string, number> {
	const result = new Map<string, number>();
	const visit = (items: FilesStateLike[]) => {
		for (const item of items) {
			if (item.isFile) {
				if (isManagedBackupPath(item.path))
					result.set(normalizeBackupPath(item.path), item.size ?? 0);
			} else if (item.files) visit(item.files);
		}
	};
	visit(state.files ?? []);
	return result;
}

export function collectBackupDirectories(paths: string[]): string[] {
	const directories = new Set<string>();
	for (const path of paths) {
		const segments = normalizeBackupPath(path).slice(1).split('/');
		for (let i = 1; i < segments.length; i++) directories.add(`/${segments.slice(0, i).join('/')}`);
	}
	return [...directories].sort(
		(a, b) => a.split('/').length - b.split('/').length || a.localeCompare(b)
	);
}

export function decodeBackupText(bytes: Uint8Array, path: string): string {
	let text: string;
	try {
		text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
	} catch {
		throw new Error(`${path} is not UTF-8 text; the File Manager API cannot restore it`);
	}
	if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(text)) {
		throw new Error(`${path} contains control bytes the File Manager API cannot restore`);
	}
	return text;
}

export function decodeBackupTextForFileManager(bytes: Uint8Array, path: string): string {
	const text = decodeBackupText(bytes, path);
	const item = {
		path,
		name: path.slice(path.lastIndexOf('/') + 1),
		isFile: true,
		contents: text
	};
	for (const operation of ['news', 'updates']) {
		const body = JSON.stringify({ [operation]: [item] });
		if (new TextEncoder().encode(body).length > CONFIG_BACKUP_MAX_REQUEST_BYTES) {
			throw new Error(`${path} is too large for the File Manager API`);
		}
	}
	return text;
}

export function validateBackupHostname(value: string): string {
	const hostname = value.trim();
	if (
		hostname.length < 3 ||
		hostname.length > 32 ||
		!/^[a-zA-Z0-9](?:[a-zA-Z0-9-]*[a-zA-Z0-9])$/.test(hostname)
	) {
		throw new Error('Hostname must be 3–32 letters, numbers, or hyphens, without edge hyphens');
	}
	return hostname;
}

export function getBackupHostname(files: BackupFile[]): string | null {
	for (const path of [CONFIG_BACKUP_WIFI_SETTINGS_PATH, CONFIG_BACKUP_ETHERNET_SETTINGS_PATH]) {
		const file = files.find((candidate) => candidate.path === path);
		if (!file) continue;
		try {
			const settings = JSON.parse(decodeBackupText(file.bytes, file.path)) as {
				hostname?: unknown;
			};
			if (typeof settings.hostname !== 'string') throw new Error();
			if (!settings.hostname.trim()) continue;
			return validateBackupHostname(settings.hostname);
		} catch {
			throw new Error(`${file.path} does not contain a valid hostname`);
		}
	}
	return null;
}

export function withBackupHostname(files: BackupFile[], hostname: string): BackupFile[] {
	const validated = validateBackupHostname(hostname);
	const encoder = new TextEncoder();
	return files.map((file) => {
		if (
			file.path !== CONFIG_BACKUP_WIFI_SETTINGS_PATH &&
			file.path !== CONFIG_BACKUP_ETHERNET_SETTINGS_PATH
		)
			return file;
		let settings: Record<string, unknown>;
		try {
			settings = JSON.parse(decodeBackupText(file.bytes, file.path)) as Record<string, unknown>;
		} catch {
			throw new Error(`${file.path} is not valid JSON`);
		}
		settings.hostname = validated;
		const bytes = encoder.encode(JSON.stringify(settings, null, 2));
		decodeBackupTextForFileManager(bytes, file.path);
		return { path: file.path, bytes };
	});
}

function sameDeviceGroup(a: string, b: string): boolean {
	const grouped = (base: string, device: string) => {
		const split = base.lastIndexOf('-');
		return split >= 0 && device.startsWith(base.slice(0, split)) && device[split] === '-';
	};
	return grouped(a, b) || grouped(b, a);
}

export function getBackupCloneWarnings(
	files: BackupFile[],
	savedHostname: string,
	restoreHostname: string
): string[] {
	const warnings: string[] = [];
	const sameHostname = savedHostname.toLowerCase() === restoreHostname.toLowerCase();
	if (sameHostname) {
		warnings.push('Keeping the saved hostname will conflict if the original board is online.');
	} else if (sameDeviceGroup(savedHostname.toLowerCase(), restoreHostname.toLowerCase())) {
		warnings.push('The new hostname remains in the same MoonLight device group as the saved one.');
	}
	for (const file of files) {
		if (
			file.path !== CONFIG_BACKUP_WIFI_SETTINGS_PATH &&
			file.path !== CONFIG_BACKUP_ETHERNET_SETTINGS_PATH &&
			file.path !== CONFIG_BACKUP_MQTT_SETTINGS_PATH &&
			file.path !== CONFIG_BACKUP_AP_SETTINGS_PATH
		)
			continue;
		try {
			const settings = JSON.parse(decodeBackupText(file.bytes, file.path)) as Record<
				string,
				unknown
			>;
			if (
				file.path === CONFIG_BACKUP_MQTT_SETTINGS_PATH &&
				settings.enabled === true &&
				typeof settings.client_id === 'string' &&
				settings.client_id
			) {
				warnings.push(
					`MQTT client ID \"${settings.client_id}\" is still shared with the saved board.`
				);
			}
			if (
				(file.path === CONFIG_BACKUP_ETHERNET_SETTINGS_PATH &&
					settings.static_ip_config === true) ||
				(file.path === CONFIG_BACKUP_WIFI_SETTINGS_PATH &&
					Array.isArray(settings.wifi_networks) &&
					settings.wifi_networks.some(
						(network) =>
							typeof network === 'object' &&
							network !== null &&
							(network as Record<string, unknown>).static_ip_config === true
					))
			) {
				warnings.push('A restored static IP can conflict if both boards use the same network.');
			}
			if (
				file.path === CONFIG_BACKUP_AP_SETTINGS_PATH &&
				settings.provision_mode !== 2 &&
				typeof settings.ssid === 'string' &&
				settings.ssid
			) {
				warnings.push(`AP SSID \"${settings.ssid}\" is still shared with the saved board.`);
			}
		} catch {
			// The archive parser and File Manager text validation report malformed files separately.
		}
	}
	return [...new Set(warnings)];
}

function writeU16(view: DataView, offset: number, value: number) {
	view.setUint16(offset, value, true);
}
function writeU32(view: DataView, offset: number, value: number) {
	view.setUint32(offset, value >>> 0, true);
}
function readU16(view: DataView, offset: number) {
	return view.getUint16(offset, true);
}
function readU32(view: DataView, offset: number) {
	return view.getUint32(offset, true);
}

const crcTable = (() => {
	const table = new Uint32Array(256);
	for (let i = 0; i < table.length; i++) {
		let value = i;
		for (let bit = 0; bit < 8; bit++) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
		table[i] = value >>> 0;
	}
	return table;
})();

export function crc32(bytes: Uint8Array): number {
	let crc = 0xffffffff;
	for (const byte of bytes) crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
	return (crc ^ 0xffffffff) >>> 0;
}

const sha256RoundConstants = new Uint32Array([
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
]);

function rotateRight(value: number, bits: number) {
	return (value >>> bits) | (value << (32 - bits));
}

// HTTP AP pages are not secure contexts on many browsers, so SubtleCrypto may be unavailable.
function sha256Fallback(bytes: Uint8Array): string {
	const paddedLength = Math.ceil((bytes.length + 9) / 64) * 64;
	const padded = new Uint8Array(paddedLength);
	padded.set(bytes);
	padded[bytes.length] = 0x80;
	const lengthView = new DataView(padded.buffer);
	const bitLengthHigh = Math.floor((bytes.length * 8) / 0x100000000);
	const bitLengthLow = (bytes.length * 8) >>> 0;
	lengthView.setUint32(paddedLength - 8, bitLengthHigh >>> 0, false);
	lengthView.setUint32(paddedLength - 4, bitLengthLow, false);
	const state = new Uint32Array([
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	]);
	const words = new Uint32Array(64);
	for (let chunk = 0; chunk < padded.length; chunk += 64) {
		const view = new DataView(padded.buffer, chunk, 64);
		for (let i = 0; i < 16; i++) words[i] = view.getUint32(i * 4, false);
		for (let i = 16; i < 64; i++) {
			const s0 =
				rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >>> 3);
			const s1 =
				rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >>> 10);
			words[i] = (words[i - 16] + s0 + words[i - 7] + s1) >>> 0;
		}
		let [a, b, c, d, e, f, g, h] = state;
		for (let i = 0; i < 64; i++) {
			const S1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
			const ch = (e & f) ^ (~e & g);
			const t1 = (h + S1 + ch + sha256RoundConstants[i] + words[i]) >>> 0;
			const S0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
			const maj = (a & b) ^ (a & c) ^ (b & c);
			const t2 = (S0 + maj) >>> 0;
			h = g;
			g = f;
			f = e;
			e = (d + t1) >>> 0;
			d = c;
			c = b;
			b = a;
			a = (t1 + t2) >>> 0;
		}
		state[0] = (state[0] + a) >>> 0;
		state[1] = (state[1] + b) >>> 0;
		state[2] = (state[2] + c) >>> 0;
		state[3] = (state[3] + d) >>> 0;
		state[4] = (state[4] + e) >>> 0;
		state[5] = (state[5] + f) >>> 0;
		state[6] = (state[6] + g) >>> 0;
		state[7] = (state[7] + h) >>> 0;
	}
	return [...state].map((value) => value.toString(16).padStart(8, '0')).join('');
}

async function sha256(bytes: Uint8Array): Promise<string> {
	if (globalThis.crypto?.subtle) {
		const input = new ArrayBuffer(bytes.byteLength);
		new Uint8Array(input).set(bytes);
		const digest = await globalThis.crypto.subtle.digest('SHA-256', input);
		return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, '0')).join('');
	}
	return sha256Fallback(bytes);
}

function concat(parts: Uint8Array[]): Uint8Array {
	const result = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
	let offset = 0;
	for (const part of parts) {
		result.set(part, offset);
		offset += part.length;
	}
	return result;
}

function zipEntryName(path: string): string {
	const normalized = normalizeBackupPath(path).slice(1);
	if (normalized === CONFIG_BACKUP_MANIFEST) throw new Error('Reserved backup manifest path');
	return normalized;
}

export async function createConfigBackup(
	files: BackupFile[],
	createdAt = new Date().toISOString()
): Promise<Blob> {
	if (files.length > CONFIG_BACKUP_MAX_FILES) throw new Error('Backup contains too many files');
	const normalized = files
		.map((file) => ({ path: normalizeBackupPath(file.path), bytes: new Uint8Array(file.bytes) }))
		.sort((a, b) => a.path.localeCompare(b.path));
	const paths = new Set<string>();
	let total = 0;
	for (const file of normalized) {
		if (!isManagedBackupPath(file.path) || paths.has(file.path))
			throw new Error(`Invalid backup file: ${file.path}`);
		if (file.bytes.length > CONFIG_BACKUP_MAX_FILE_BYTES)
			throw new Error(`Backup file is too large: ${file.path}`);
		decodeBackupTextForFileManager(file.bytes, file.path);
		paths.add(file.path);
		total += file.bytes.length;
	}
	const missingRequired = CONFIG_BACKUP_REQUIRED_PATHS.filter((path) => !paths.has(path));
	if (missingRequired.length)
		throw new Error(`Full backup is missing required settings: ${missingRequired.join(', ')}`);
	if (total > CONFIG_BACKUP_MAX_BYTES) throw new Error('Backup is too large');
	const manifest: BackupManifest = {
		schema: CONFIG_BACKUP_SCHEMA,
		version: CONFIG_BACKUP_VERSION,
		fullBackupVersion: CONFIG_BACKUP_FULL_VERSION,
		createdAt,
		files: []
	};
	for (const file of normalized)
		manifest.files.push({
			path: file.path,
			size: file.bytes.length,
			sha256: await sha256(file.bytes)
		});
	const encoder = new TextEncoder();
	const entries = [
		{ path: CONFIG_BACKUP_MANIFEST, bytes: encoder.encode(JSON.stringify(manifest, null, 2)) },
		...normalized.map((file) => ({ path: zipEntryName(file.path), bytes: file.bytes }))
	];
	const localParts: Uint8Array[] = [],
		centralParts: Uint8Array[] = [];
	let offset = 0;
	for (const entry of entries) {
		const name = encoder.encode(entry.path),
			crc = crc32(entry.bytes);
		if (name.length > 0xffff || entry.bytes.length > 0xffffffff || offset > 0xffffffff)
			throw new Error('Backup ZIP exceeds ZIP32 limits');
		const local = new Uint8Array(30 + name.length),
			localView = new DataView(local.buffer);
		writeU32(localView, 0, 0x04034b50);
		writeU16(localView, 4, 20);
		writeU16(localView, 6, 0x0800);
		writeU16(localView, 8, 0);
		writeU32(localView, 14, crc);
		writeU32(localView, 18, entry.bytes.length);
		writeU32(localView, 22, entry.bytes.length);
		writeU16(localView, 26, name.length);
		local.set(name, 30);
		localParts.push(local, entry.bytes);
		const central = new Uint8Array(46 + name.length),
			centralView = new DataView(central.buffer);
		writeU32(centralView, 0, 0x02014b50);
		writeU16(centralView, 4, 20);
		writeU16(centralView, 6, 20);
		writeU16(centralView, 8, 0x0800);
		writeU16(centralView, 10, 0);
		writeU32(centralView, 16, crc);
		writeU32(centralView, 20, entry.bytes.length);
		writeU32(centralView, 24, entry.bytes.length);
		writeU16(centralView, 28, name.length);
		writeU32(centralView, 42, offset);
		central.set(name, 46);
		centralParts.push(central);
		offset += local.length + entry.bytes.length;
	}
	const centralDirectory = concat(centralParts),
		end = new Uint8Array(22),
		endView = new DataView(end.buffer);
	writeU32(endView, 0, 0x06054b50);
	writeU16(endView, 8, entries.length);
	writeU16(endView, 10, entries.length);
	writeU32(endView, 12, centralDirectory.length);
	writeU32(endView, 16, offset);
	const archive = concat([...localParts, centralDirectory, end]);
	const archiveBuffer = new ArrayBuffer(archive.byteLength);
	new Uint8Array(archiveBuffer).set(archive);
	return new Blob([archiveBuffer], { type: 'application/zip' });
}

function findEndOfCentralDirectory(bytes: Uint8Array): number {
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const minimum = Math.max(0, bytes.length - 0xffff - 22);
	for (let offset = bytes.length - 22; offset >= minimum; offset--)
		if (view.getUint32(offset, true) === 0x06054b50) return offset;
	throw new Error('Invalid ZIP: end record not found');
}

function readZipName(bytes: Uint8Array, offset: number, length: number): string {
	if (offset < 0 || offset + length > bytes.length) throw new Error('Invalid ZIP filename bounds');
	try {
		return new TextDecoder('utf-8', { fatal: true }).decode(bytes.slice(offset, offset + length));
	} catch {
		throw new Error('Invalid ZIP filename encoding');
	}
}

function normalizeZipName(name: string): string {
	if (!name || name.startsWith('/') || name.includes('\\') || name.includes('\0'))
		throw new Error(`Invalid ZIP path: ${name}`);
	const segments = name.split('/');
	if (segments.some((segment) => !segment || segment === '.' || segment === '..'))
		throw new Error(`Invalid ZIP path: ${name}`);
	return normalizeBackupPath(`/${segments.join('/')}`);
}

export async function parseConfigBackup(
	input: Blob | ArrayBuffer | Uint8Array
): Promise<ParsedBackup> {
	if (input instanceof Blob && input.size > CONFIG_BACKUP_MAX_ARCHIVE_BYTES)
		throw new Error('Backup is too large');
	const bytes =
		input instanceof Blob
			? new Uint8Array(await input.arrayBuffer())
			: input instanceof Uint8Array
				? input
				: new Uint8Array(input);
	if (bytes.length > CONFIG_BACKUP_MAX_ARCHIVE_BYTES) throw new Error('Backup is too large');
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength),
		eocd = findEndOfCentralDirectory(bytes);
	if (eocd + 22 + readU16(view, eocd + 20) !== bytes.length)
		throw new Error('ZIP has an unsupported trailing comment');
	if (
		readU16(view, eocd + 4) !== 0 ||
		readU16(view, eocd + 6) !== 0 ||
		readU16(view, eocd + 8) !== readU16(view, eocd + 10)
	)
		throw new Error('Multi-disk ZIP archives are not supported');
	const count = readU16(view, eocd + 10),
		centralSize = readU32(view, eocd + 12),
		centralOffset = readU32(view, eocd + 16),
		centralEnd = centralOffset + centralSize;
	if (!count || count > CONFIG_BACKUP_MAX_FILES + 1 || centralOffset > eocd || centralEnd > eocd)
		throw new Error('Invalid ZIP directory');
	const files: BackupFile[] = [],
		names = new Set<string>(),
		entries = new Map<string, Uint8Array>();
	let cursor = centralOffset,
		total = 0;
	for (let index = 0; index < count; index++) {
		if (cursor + 46 > centralEnd || readU32(view, cursor) !== 0x02014b50)
			throw new Error('Invalid ZIP central entry');
		const flags = readU16(view, cursor + 8),
			method = readU16(view, cursor + 10),
			crc = readU32(view, cursor + 16),
			compressedSize = readU32(view, cursor + 20),
			uncompressedSize = readU32(view, cursor + 24);
		const nameLength = readU16(view, cursor + 28),
			extraLength = readU16(view, cursor + 30),
			commentLength = readU16(view, cursor + 32),
			localOffset = readU32(view, cursor + 42);
		const next = cursor + 46 + nameLength + extraLength + commentLength;
		if (
			next > centralEnd ||
			(flags & 0x0009) !== 0 ||
			(flags & ~0x0800) !== 0 ||
			method !== 0 ||
			compressedSize !== uncompressedSize ||
			compressedSize > CONFIG_BACKUP_MAX_FILE_BYTES
		)
			throw new Error('ZIP uses unsupported flags or compression');
		const rawName = readZipName(bytes, cursor + 46, nameLength),
			key = rawName === CONFIG_BACKUP_MANIFEST ? CONFIG_BACKUP_MANIFEST : normalizeZipName(rawName);
		if (names.has(key)) throw new Error(`Duplicate backup path: ${key}`);
		names.add(key);
		if (localOffset + 30 > bytes.length || readU32(view, localOffset) !== 0x04034b50)
			throw new Error('Invalid ZIP local entry');
		const localFlags = readU16(view, localOffset + 6),
			localMethod = readU16(view, localOffset + 8),
			localCrc = readU32(view, localOffset + 14),
			localCompressedSize = readU32(view, localOffset + 18),
			localUncompressedSize = readU32(view, localOffset + 22),
			localNameLength = readU16(view, localOffset + 26),
			localExtraLength = readU16(view, localOffset + 28);
		const localName = readZipName(bytes, localOffset + 30, localNameLength),
			dataOffset = localOffset + 30 + localNameLength + localExtraLength;
		if (
			localName !== rawName ||
			localFlags !== flags ||
			localMethod !== method ||
			localCrc !== crc ||
			localCompressedSize !== compressedSize ||
			localUncompressedSize !== uncompressedSize ||
			dataOffset + compressedSize > bytes.length
		)
			throw new Error(`Invalid ZIP local entry: ${rawName}`);
		const content = bytes.slice(dataOffset, dataOffset + compressedSize);
		if (crc32(content) !== crc) throw new Error(`CRC mismatch for ${rawName}`);
		entries.set(key, content);
		if (key !== CONFIG_BACKUP_MANIFEST) {
			total += content.length;
			if (total > CONFIG_BACKUP_MAX_BYTES) throw new Error('Backup is too large');
			files.push({ path: key, bytes: content });
		}
		cursor = next;
	}
	if (cursor !== centralEnd || !entries.has(CONFIG_BACKUP_MANIFEST))
		throw new Error('Backup manifest is missing');
	let manifest: BackupManifest;
	try {
		manifest = JSON.parse(
			new TextDecoder('utf-8', { fatal: true }).decode(entries.get(CONFIG_BACKUP_MANIFEST)!)
		) as BackupManifest;
	} catch {
		throw new Error('Backup manifest is not valid UTF-8 JSON');
	}
	if (
		manifest.schema !== CONFIG_BACKUP_SCHEMA ||
		manifest.version !== CONFIG_BACKUP_VERSION ||
		manifest.fullBackupVersion !== CONFIG_BACKUP_FULL_VERSION ||
		!Array.isArray(manifest.files) ||
		manifest.files.length !== files.length ||
		manifest.files.length > CONFIG_BACKUP_MAX_FILES
	)
		throw new Error('Unsupported backup schema');
	const manifestPaths = new Set<string>(),
		fileMap = new Map(files.map((file) => [file.path, file]));
	for (const entry of manifest.files) {
		const path = normalizeBackupPath(entry.path);
		if (
			!isManagedBackupPath(path) ||
			manifestPaths.has(path) ||
			!Number.isSafeInteger(entry.size) ||
			entry.size < 0 ||
			entry.size > CONFIG_BACKUP_MAX_FILE_BYTES ||
			!/^[0-9a-f]{64}$/i.test(entry.sha256)
		)
			throw new Error('Invalid backup manifest entry');
		const file = fileMap.get(path);
		if (
			!file ||
			file.bytes.length !== entry.size ||
			(await sha256(file.bytes)).toLowerCase() !== entry.sha256.toLowerCase()
		)
			throw new Error(`Manifest mismatch for ${path}`);
		manifestPaths.add(path);
	}
	if (files.some((file) => !manifestPaths.has(file.path)))
		throw new Error('Backup contains an unmanifested file');
	const missingRequired = CONFIG_BACKUP_REQUIRED_PATHS.filter((path) => !manifestPaths.has(path));
	if (missingRequired.length)
		throw new Error(`Full backup is missing required settings: ${missingRequired.join(', ')}`);
	return { manifest, files };
}

export async function sha256Hex(bytes: Uint8Array): Promise<string> {
	return sha256(bytes);
}
