<!--
   @title     MoonBase
   @file      FileManager.svelte
   @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
   @Authors   https://github.com/MoonModules/MoonLight/commits/main
   @Doc       https://moonmodules.org/MoonLight/moonbase/FileManager/
   @Copyright © 2026 GitHub MoonLight Commit Authors
   @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
   @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
-->

<script lang="ts">
	import { user } from '$lib/stores/user';
	import { page } from '$app/state';
	import { notifications } from '$lib/components/toasts/notifications';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import FilesIcon from '~icons/tabler/files';
	import Help from '~icons/tabler/help';
	import Download from '~icons/tabler/download';
	import Upload from '~icons/tabler/upload';
	import {
		collectBackupDirectories,
		createConfigBackup,
		decodeBackupTextForFileManager,
		discoverBackupScriptPaths,
		getBackupProbePaths,
		getBackupCloneWarnings,
		getBackupHostname,
		parseConfigBackup,
		sha256Hex,
		validateBackupHostname,
		withBackupHostname,
		type BackupFile
	} from './configBackup';

	let backupBusy = $state(false);
	let backupFileInput: HTMLInputElement = $state()!;

	function authHeaders(contentType?: string) {
		return {
			Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
			...(contentType ? { 'Content-Type': contentType } : {})
		};
	}

	async function postFileManager(data: Record<string, unknown>) {
		const response = await fetch('/rest/FileManager', {
			method: 'POST',
			headers: authHeaders('application/json'),
			body: JSON.stringify(data)
		});
		if (!response.ok) throw new Error(`File Manager returned HTTP ${response.status}`);
	}

	async function getFilesystemUsage() {
		const response = await fetch('/rest/systemStatus', {
			headers: authHeaders(),
			cache: 'no-store'
		});
		if (!response.ok) throw new Error(`System status returned HTTP ${response.status}`);
		const status = (await response.json()) as { fs_total?: unknown; fs_used?: unknown };
		if (typeof status.fs_total !== 'number' || typeof status.fs_used !== 'number') {
			throw new Error('System status did not include filesystem usage');
		}
		return { total: status.fs_total, used: status.fs_used };
	}

	function fileUrl(filePath: string) {
		return '/rest/file/' + filePath.slice(1).split('/').map(encodeURIComponent).join('/');
	}

	async function probeFile(filePath: string): Promise<BackupFile | null> {
		const response = await fetch(fileUrl(filePath), {
			headers: authHeaders(),
			cache: 'no-store'
		});
		if (response.status === 404) return null;
		if (!response.ok) throw new Error(`Could not read ${filePath} (HTTP ${response.status})`);
		return { path: filePath, bytes: new Uint8Array(await response.arrayBuffer()) };
	}

	async function probeFiles(paths: string[]): Promise<BackupFile[]> {
		const files: BackupFile[] = [];
		for (const path of paths) {
			const file = await probeFile(path);
			if (file) files.push(file);
		}
		return files;
	}

	function downloadBlob(blob: Blob, filename: string) {
		const url = URL.createObjectURL(blob);
		const anchor = document.createElement('a');
		anchor.href = url;
		anchor.download = filename;
		document.body.appendChild(anchor);
		anchor.click();
		anchor.remove();
		URL.revokeObjectURL(url);
	}

	async function exportConfigBackup() {
		if (backupBusy) return;
		backupBusy = true;
		try {
			const saved = await fetch('/rest/saveConfig', { method: 'POST', headers: authHeaders() });
			if (!saved.ok) throw new Error(`Could not flush live configuration (HTTP ${saved.status})`);
			const files = await probeFiles(getBackupProbePaths());
			const existingPaths = new Set(files.map((file) => file.path));
			files.push(
				...(await probeFiles(
					discoverBackupScriptPaths(files).filter((path) => !existingPaths.has(path))
				))
			);
			downloadBlob(
				await createConfigBackup(files),
				`moonlight-config-${new Date().toISOString().replace(/[:.]/g, '-')}.zip`
			);
			notifications.success(`Exported ${files.length} files.`, 4000);
		} catch (error) {
			console.error('Configuration export failed:', error);
			notifications.error(
				`Configuration export failed: ${error instanceof Error ? error.message : 'unknown error'}`,
				6000
			);
		} finally {
			backupBusy = false;
		}
	}

	function fileName(filePath: string) {
		return filePath.slice(filePath.lastIndexOf('/') + 1);
	}

	async function readFileBytes(filePath: string) {
		const response = await fetch(fileUrl(filePath), { headers: authHeaders(), cache: 'no-store' });
		if (!response.ok) throw new Error(`Could not read ${filePath} (HTTP ${response.status})`);
		return new Uint8Array(await response.arrayBuffer());
	}

	async function importConfigBackup(event: Event) {
		const input = event.target as HTMLInputElement;
		const selected = input.files?.[0];
		input.value = '';
		if (!selected || backupBusy) return;
		backupBusy = true;
		try {
			const backup = await parseConfigBackup(selected);
			let restoreFiles = backup.files;
			const savedHostname = getBackupHostname(restoreFiles);
			let restoreHostname = savedHostname;
			if (savedHostname) {
				const requestedHostname = window.prompt(
					'Device hostname for this restore. Keep the saved name for a replacement board, or change the full name for a second online board to avoid mDNS and group-sync conflicts.',
					savedHostname
				);
				if (requestedHostname === null) return;
				restoreHostname = validateBackupHostname(requestedHostname);
				restoreFiles = withBackupHostname(restoreFiles, restoreHostname);
			}
			const cloneWarnings =
				savedHostname && restoreHostname
					? getBackupCloneWarnings(restoreFiles, savedHostname, restoreHostname)
					: [];
			const restoreText = new Map(
				restoreFiles.map((file) => [
					file.path,
					decodeBackupTextForFileManager(file.bytes, file.path)
				])
			);
			if (
				!window.confirm(
					`Restore ${restoreFiles.length} files${restoreHostname ? ` as ${restoreHostname}` : ''}? Wi-Fi/AP credentials are included and the device will reboot.${cloneWarnings.length ? `\n\nWarning:\n- ${cloneWarnings.join('\n- ')}` : ''}`
				)
			)
				return;

			const currentFiles = await probeFiles(
				getBackupProbePaths(restoreFiles.map((file) => file.path))
			);
			const currentPaths = new Set(currentFiles.map((file) => file.path));
			const currentSizes = new Map(currentFiles.map((file) => [file.path, file.bytes.length]));
			const backupPaths = new Set(restoreFiles.map((file) => file.path));
			const expectedHashes = new Map(
				await Promise.all(
					restoreFiles.map(async (file) => [file.path, await sha256Hex(file.bytes)] as const)
				)
			);
			const requiredGrowth = restoreFiles.reduce(
				(total, file) =>
					total + Math.max(0, file.bytes.length - (currentSizes.get(file.path) ?? 0)),
				0
			);
			const usage = await getFilesystemUsage();
			if (requiredGrowth + 4096 > usage.total - usage.used) {
				throw new Error('Not enough free filesystem space to restore this backup safely');
			}
			for (const directory of collectBackupDirectories(restoreFiles.map((file) => file.path))) {
				await postFileManager({
					news: [{ path: directory, name: fileName(directory), isFile: false, contents: '' }]
				});
			}
			for (const file of restoreFiles) {
				const item = {
					path: file.path,
					name: fileName(file.path),
					isFile: true,
					contents: restoreText.get(file.path)!
				};
				await postFileManager(currentPaths.has(file.path) ? { updates: [item] } : { news: [item] });
			}

			for (const file of restoreFiles) {
				const expected = expectedHashes.get(file.path);
				if (
					!expected ||
					(await sha256Hex(await readFileBytes(file.path))).toLowerCase() !== expected.toLowerCase()
				) {
					throw new Error(
						`Restore verification failed for ${file.path}; the device was not rebooted`
					);
				}
			}

			for (const currentPath of [...currentPaths]
				.filter((candidate) => !backupPaths.has(candidate))
				.sort((a, b) => b.length - a.length)) {
				await postFileManager({ deletes: [{ path: currentPath, isFile: true }] });
			}

			for (const stalePath of [...currentPaths].filter((path) => !backupPaths.has(path))) {
				if (await probeFile(stalePath)) {
					throw new Error(
						`Restore verification found stale file ${stalePath}; the device was not rebooted`
					);
				}
			}

			const restart = await fetch('/rest/restart', { method: 'POST', headers: authHeaders() });
			if (!restart.ok)
				throw new Error(`Restore completed but reboot failed (HTTP ${restart.status})`);
			notifications.success(
				'Configuration restored. Reconnect after reboot, verify it, then press ⭐ to update the golden copy.',
				8000
			);
			window.setTimeout(() => window.location.reload(), 1500);
		} catch (error) {
			console.error('Configuration import failed:', error);
			notifications.error(
				`Configuration import failed: ${error instanceof Error ? error.message : 'unknown error'}`,
				8000
			);
		} finally {
			backupBusy = false;
		}
	}
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<FilesIcon class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Files</span>
		<div class="absolute right-5">
			<a
				href="https://{page.data.github.split('/')[0]}.github.io/{page.data.github.split(
					'/'
				)[1]}{page.url.pathname}"
				target="_blank"
				rel="noopener noreferrer"
				title="Documentation"><Help class="mr-2 h-6 w-6 shrink-0 self-end" /></a
			>
		</div>
		<!-- 🌙 link to docs -->
	{/snippet}

	{#if !page.data.features.security || $user.admin}
		<div class="bg-base-200 relative grid w-full max-w-2xl self-center overflow-hidden shadow-lg">
			<div class="flex h-16 w-full items-center justify-between space-x-3 p-0 text-xl font-medium">
				Configuration backup
			</div>
			<div class="flex flex-wrap gap-2 pb-4">
				<button
					class="btn btn-secondary text-secondary-content btn-md"
					title="Export configuration backup"
					aria-label="Export configuration backup"
					onclick={exportConfigBackup}
					disabled={backupBusy}
				>
					<Download class="h-6 w-6" />
				</button>
				<button
					class="btn btn-secondary text-secondary-content btn-md"
					title="Import configuration backup"
					aria-label="Import configuration backup"
					onclick={() => backupFileInput?.click()}
					disabled={backupBusy}
				>
					<Upload class="h-6 w-6" />
				</button>
				<input
					class="hidden"
					bind:this={backupFileInput}
					type="file"
					accept=".zip,application/zip"
					onchange={importConfigBackup}
				/>
			</div>
		</div>
	{/if}
</SettingsCard>
