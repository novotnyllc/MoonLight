import { redirect } from '@sveltejs/kit';
import type { PageLoad } from './$types';

const CONTROL_ROUTE = '/moonbase/module?group=moonlight&module=lightscontrol';

export function hasConfiguredLights(info: unknown): boolean {
	return (
		typeof info === 'object' &&
		info !== null &&
		typeof (info as { nrOfLights?: unknown }).nrOfLights === 'number' &&
		(info as { nrOfLights: number }).nrOfLights > 0
	);
}

export function authorizationHeader(security: boolean, storedUser: string | null): string {
	if (!security) return 'Basic';
	try {
		const token = (JSON.parse(storedUser ?? '{}') as { bearer_token?: unknown }).bearer_token;
		return 'Bearer ' + (typeof token === 'string' ? token : '');
	} catch {
		return 'Bearer ';
	}
}

export const load: PageLoad = async ({ fetch, parent }) => {
	let info: unknown;

	try {
		const { features } = await parent();
		const response = await fetch('/rest/moonlightinfo', {
			headers: {
				Authorization: authorizationHeader(features.security, localStorage.getItem('user'))
			}
		});
		if (!response.ok) return {};
		info = await response.json();
	} catch {
		return {};
	}

	if (hasConfiguredLights(info)) redirect(307, CONTROL_ROUTE);
	return {};
};
