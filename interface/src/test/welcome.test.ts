import { describe, expect, it } from 'vitest';
import { authorizationHeader, hasConfiguredLights } from '../routes/+page';

describe('configured root route', () => {
	it('redirects only after a light layout exists', () => {
		expect(hasConfiguredLights({ nrOfLights: 95 })).toBe(true);
		expect(hasConfiguredLights({ nrOfLights: 0 })).toBe(false);
		expect(hasConfiguredLights({})).toBe(false);
	});

	it('authenticates the configured-state request when security is enabled', () => {
		expect(authorizationHeader(true, '{"bearer_token":"token"}')).toBe('Bearer token');
		expect(authorizationHeader(false, null)).toBe('Basic');
	});
});
