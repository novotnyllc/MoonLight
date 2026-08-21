import { describe, expect, it } from 'vitest';
import {
	CONFIG_BACKUP_CANDIDATE_PATHS,
	CONFIG_BACKUP_FULL_VERSION,
	CONFIG_BACKUP_MANIFEST,
	CONFIG_BACKUP_MAX_ARCHIVE_BYTES,
	CONFIG_BACKUP_MAX_FILE_BYTES,
	CONFIG_BACKUP_REQUIRED_PATHS,
	collectBackupDirectories,
	collectBackupFiles,
	createConfigBackup,
	crc32,
	decodeBackupText,
	decodeBackupTextForFileManager,
	discoverBackupScriptPaths,
	getBackupProbePaths,
	getBackupCloneWarnings,
	getBackupHostname,
	normalizeBackupPath,
	parseConfigBackup,
	sha256Hex,
	validateBackupHostname,
	withBackupHostname
} from '../routes/moonbase/filemanager/configBackup';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function fullBackupFiles(
	...extra: { path: string; bytes: Uint8Array }[]
): { path: string; bytes: Uint8Array }[] {
	return [
		{ path: '/.config/wifiSettings.json', bytes: encoder.encode('{"hostname":"white-vest"}') },
		{ path: '/.config/apSettings.json', bytes: encoder.encode('{}') },
		...extra
	];
}

async function rewriteStoredArchive(
	archive: Blob,
	replacements: [string, string][]
): Promise<Uint8Array> {
	const bytes = new Uint8Array(await archive.arrayBuffer());
	for (const [from, to] of replacements) {
		if (from.length !== to.length) throw new Error('Test ZIP rewrites must preserve length');
		const source = encoder.encode(from),
			target = encoder.encode(to);
		let replaced = 0;
		for (let offset = 0; offset <= bytes.length - source.length; offset++) {
			if (source.every((byte, index) => bytes[offset + index] === byte)) {
				bytes.set(target, offset);
				replaced++;
				offset += source.length - 1;
			}
		}
		if (!replaced) throw new Error(`Test ZIP text not found: ${from}`);
	}

	const view = new DataView(bytes.buffer);
	let localOffset = 0,
		manifestLocal = -1,
		manifestData = -1,
		manifestSize = 0;
	while (localOffset + 30 <= bytes.length && view.getUint32(localOffset, true) === 0x04034b50) {
		const size = view.getUint32(localOffset + 18, true),
			nameLength = view.getUint16(localOffset + 26, true),
			extraLength = view.getUint16(localOffset + 28, true),
			name = decoder.decode(bytes.slice(localOffset + 30, localOffset + 30 + nameLength)),
			dataOffset = localOffset + 30 + nameLength + extraLength;
		if (name === CONFIG_BACKUP_MANIFEST) {
			manifestLocal = localOffset;
			manifestData = dataOffset;
			manifestSize = size;
		}
		localOffset = dataOffset + size;
	}
	if (manifestLocal < 0) throw new Error('Test ZIP manifest not found');
	const manifestCrc = crc32(bytes.slice(manifestData, manifestData + manifestSize));
	view.setUint32(manifestLocal + 14, manifestCrc, true);
	for (let offset = localOffset; offset + 46 <= bytes.length; offset++) {
		if (view.getUint32(offset, true) !== 0x02014b50) continue;
		const nameLength = view.getUint16(offset + 28, true),
			name = decoder.decode(bytes.slice(offset + 46, offset + 46 + nameLength));
		if (name === CONFIG_BACKUP_MANIFEST) {
			view.setUint32(offset + 16, manifestCrc, true);
			break;
		}
	}
	return bytes;
}

describe('configuration backup archive', () => {
	it('enumerates the finite portable configuration candidates', () => {
		expect(CONFIG_BACKUP_CANDIDATE_PATHS).toHaveLength(41);
		expect(CONFIG_BACKUP_CANDIDATE_PATHS).toContain('/.config/wifiSettings.json');
		expect(CONFIG_BACKUP_CANDIDATE_PATHS).toContain('/.config/lightscontrol.json');
		expect(CONFIG_BACKUP_CANDIDATE_PATHS).toContain('/.config/presets/preset01.json');
		expect(CONFIG_BACKUP_CANDIDATE_PATHS).toContain('/.config/presets/preset20.json');
		expect(CONFIG_BACKUP_CANDIDATE_PATHS).toContain('/L_WhiteVest95_3D.sc');
		expect(CONFIG_BACKUP_REQUIRED_PATHS).toEqual([
			'/.config/wifiSettings.json',
			'/.config/apSettings.json'
		]);
		expect(getBackupProbePaths(['/livescripts/custom.sc', '/livescripts/custom.sc'])).toEqual([
			...CONFIG_BACKUP_CANDIDATE_PATHS,
			'/livescripts/custom.sc'
		]);
	});

	it('discovers referenced scripts recursively without transient or unsafe paths', () => {
		const files = [
			{
				path: '/.config/effects.json',
				bytes: new TextEncoder().encode(
					JSON.stringify({
						layout: 'L_Custom.sc',
						effects: [
							'/livescripts/E_Absolute.sc',
							'livescripts/E_Prefixed.sc',
							'Effects/E_Relative.sc',
							'../escape.sc',
							'/livescripts/ignored.sc.bak'
						]
					})
				)
			},
			{ path: '/.config/bad.json', bytes: new TextEncoder().encode('{') }
		];
		expect(discoverBackupScriptPaths(files)).toEqual([
			'/L_Custom.sc',
			'/livescripts/E_Absolute.sc',
			'/livescripts/E_Prefixed.sc',
			'/livescripts/Effects/E_Relative.sc'
		]);
	});

	it('computes standard SHA-256 hashes', async () => {
		expect(await sha256Hex(new TextEncoder().encode('abc'))).toBe(
			'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
		);
	});

	it('round-trips UTF-8 files with an integrity manifest', async () => {
		const archive = await createConfigBackup(
			fullBackupFiles(
				{ path: '/livescripts/night.sc', bytes: encoder.encode('effect(1);') },
				{ path: '/L_WhiteVest95.sc', bytes: encoder.encode('pixels(95);') }
			)
		);
		const parsed = await parseConfigBackup(archive);
		expect(parsed.manifest.schema).toBe('moonlight-config-backup');
		expect(parsed.manifest.fullBackupVersion).toBe(CONFIG_BACKUP_FULL_VERSION);
		expect(parsed.files.map((file) => file.path)).toEqual([
			'/.config/apSettings.json',
			'/.config/wifiSettings.json',
			'/L_WhiteVest95.sc',
			'/livescripts/night.sc'
		]);
	});

	it('rejects legacy markers and partial clean-restore archives', async () => {
		const archive = await createConfigBackup(fullBackupFiles());
		const legacy = await rewriteStoredArchive(archive, [
			['fullBackupVersion', 'notFullBackupVers']
		]);
		await expect(parseConfigBackup(legacy)).rejects.toThrow('Unsupported backup schema');

		const partial = await rewriteStoredArchive(archive, [
			['.config/apSettings.json', '.config/xxSettings.json']
		]);
		await expect(parseConfigBackup(partial)).rejects.toThrow(
			'Full backup is missing required settings: /.config/apSettings.json'
		);
		await expect(
			createConfigBackup([{ path: '/.config/wifiSettings.json', bytes: encoder.encode('{}') }])
		).rejects.toThrow('Full backup is missing required settings');
	});

	it('rejects traversal, protected, transient, and non-root-sc paths', async () => {
		expect(() => normalizeBackupPath('/.config/../wifi.json')).toThrow();
		expect(() => normalizeBackupPath('/.config/.config-golden/secret.json')).toThrow();
		expect(() => normalizeBackupPath('/livescripts/recovery/secret.sc')).toThrow();
		expect(() => normalizeBackupPath('/livescripts/night.sc.tmp')).toThrow();
		await expect(
			createConfigBackup([{ path: '/root.txt', bytes: new Uint8Array() }])
		).rejects.toThrow();
	});

	it('collects files and creates parent directories', () => {
		expect(
			collectBackupDirectories([
				'/.config/wifiSettings.json',
				'/.config/presets/1.json',
				'/livescripts/night.sc',
				'/L_WhiteVest95.sc'
			])
		).toEqual(['/.config', '/livescripts', '/.config/presets']);
		expect(
			collectBackupFiles({
				name: '/',
				path: '/',
				isFile: false,
				files: [
					{
						name: '.config',
						path: '/.config',
						isFile: false,
						files: [{ name: 'x', path: '/.config/x', isFile: true }]
					},
					{
						name: '.config-golden',
						path: '/.config-golden',
						isFile: false,
						files: [{ name: 'x', path: '/.config-golden/x', isFile: true }]
					},
					{ name: 'root.txt', path: '/root.txt', isFile: true },
					{ name: 'root.sc', path: '/root.sc', isFile: true }
				]
			})
		).toEqual(['/.config/x', '/root.sc']);
	});

	it('rejects archive corruption before returning files', async () => {
		const archive = await createConfigBackup(fullBackupFiles());
		const bytes = new Uint8Array(await archive.arrayBuffer());
		const marker = new TextEncoder().encode('{}');
		const dataOffset = bytes.findIndex((value, index) => index > 30 && value === marker[0]);
		expect(dataOffset).toBeGreaterThanOrEqual(0);
		bytes[dataOffset] ^= 0xff;
		await expect(parseConfigBackup(bytes)).rejects.toThrow();
	});

	it('rejects an oversized Blob before reading it', async () => {
		const blob = new Blob([new Uint8Array(CONFIG_BACKUP_MAX_ARCHIVE_BYTES + 1)]);
		await expect(parseConfigBackup(blob)).rejects.toThrow('Backup is too large');
	});

	it('rejects files too large for one ESP32 File Manager request', async () => {
		await expect(
			createConfigBackup(
				fullBackupFiles({
					path: '/livescripts/too-large.sc',
					bytes: new Uint8Array(CONFIG_BACKUP_MAX_FILE_BYTES + 1)
				})
			)
		).rejects.toThrow('Backup file is too large');
		expect(() =>
			decodeBackupTextForFileManager(
				new TextEncoder().encode('"'.repeat(8 * 1024)),
				'/livescripts/escaped.sc'
			)
		).toThrow('too large for the File Manager API');
	});

	it('rejects NUL and unsafe control bytes before File Manager writes', () => {
		expect(() => decodeBackupText(new Uint8Array([0x61, 0x00, 0x62]), '/.config/x.json')).toThrow(
			'contains control bytes'
		);
		expect(decodeBackupText(new TextEncoder().encode('line\n\tvalue'), '/.config/x.json')).toBe(
			'line\n\tvalue'
		);
	});

	it('defaults to the saved hostname and can prepare a conflict-free clone name', () => {
		const original = [
			{
				path: '/.config/wifiSettings.json',
				bytes: new TextEncoder().encode('{"hostname":"white-vest-next","connection_mode":1}')
			},
			{ path: '/.config/lightscontrol.json', bytes: new TextEncoder().encode('{"brightness":64}') }
		];
		expect(getBackupHostname(original)).toBe('white-vest-next');
		const clone = withBackupHostname(original, 'white-vest-spare');
		expect(getBackupHostname(clone)).toBe('white-vest-spare');
		expect(clone[1].bytes).toEqual(original[1].bytes);
		expect(() => validateBackupHostname('-bad-name')).toThrow();
		expect(validateBackupHostname('a'.repeat(32))).toBe('a'.repeat(32));
		expect(() => validateBackupHostname('a'.repeat(33))).toThrow();
		expect(getBackupCloneWarnings(original, 'white-vest-next', 'WHITE-VEST-NEXT')).toHaveLength(1);
		expect(getBackupCloneWarnings(original, 'white-vest-next', 'white-vest-spare')).toContain(
			'The new hostname remains in the same MoonLight device group as the saved one.'
		);
	});

	it('uses and rewrites the Ethernet hostname when Wi-Fi settings are absent', () => {
		const original = [
			{
				path: '/.config/ethernetSettings.json',
				bytes: new TextEncoder().encode('{"hostname":"moonlight-wired"}')
			}
		];
		expect(getBackupHostname(original)).toBe('moonlight-wired');
		expect(getBackupHostname(withBackupHostname(original, 'moonlight-spare'))).toBe(
			'moonlight-spare'
		);
	});

	it('falls back to Ethernet when the Wi-Fi hostname is empty', () => {
		expect(
			getBackupHostname([
				{
					path: '/.config/wifiSettings.json',
					bytes: encoder.encode('{"hostname":""}')
				},
				{
					path: '/.config/ethernetSettings.json',
					bytes: encoder.encode('{"hostname":"moonlight-wired"}')
				}
			])
		).toBe('moonlight-wired');
	});

	it('prefers the Wi-Fi hostname over Ethernet regardless of archive order', () => {
		expect(
			getBackupHostname([
				{
					path: '/.config/ethernetSettings.json',
					bytes: new TextEncoder().encode('{"hostname":"moonlight-wired"}')
				},
				{
					path: '/.config/wifiSettings.json',
					bytes: new TextEncoder().encode('{"hostname":"moonlight-wifi"}')
				}
			])
		).toBe('moonlight-wifi');
	});
});
