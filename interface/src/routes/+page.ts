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

export const load: PageLoad = async ({ fetch }) => {
	let info: unknown;

	try {
		const response = await fetch('/rest/moonlightinfo');
		if (!response.ok) return {};
		info = await response.json();
	} catch {
		return {};
	}

	if (hasConfiguredLights(info)) redirect(307, CONTROL_ROUTE);
	return {};
};
