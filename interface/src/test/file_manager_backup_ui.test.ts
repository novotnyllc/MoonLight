import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

const source = readFileSync(
	new URL('../routes/moonbase/filemanager/FileManager.svelte', import.meta.url),
	'utf8'
);

describe('File Manager backup UI', () => {
	it('keeps scanless backup controls and removes recursive inventory triggers', () => {
		expect(source).toContain('Export configuration backup');
		expect(source).toContain('Import configuration backup');
		expect(source).toContain('getBackupProbePaths()');
		expect(source).toContain('discoverBackupScriptPaths(files)');
		expect(source).not.toContain('/rest/FileManagerBackup');
		expect(source).not.toContain('Browse files');
		expect(source).not.toContain('getFileManagerState');
		expect(source).not.toContain("socket.on('FileManager'");
		expect(source).not.toContain('href="/rest/FileManager"');
		expect(source.match(/fetch\('\/rest\/FileManager'/g)).toHaveLength(1);
		expect(source).toMatch(/fetch\('\/rest\/FileManager',\s*\{\s*method: 'POST'/);
	});
});
