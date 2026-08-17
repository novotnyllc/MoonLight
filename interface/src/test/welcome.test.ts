import { describe, expect, it } from 'vitest';
import { hasConfiguredLights } from '../routes/+page';

describe('configured root route', () => {
	it('redirects only after a light layout exists', () => {
		expect(hasConfiguredLights({ nrOfLights: 95 })).toBe(true);
		expect(hasConfiguredLights({ nrOfLights: 0 })).toBe(false);
		expect(hasConfiguredLights({})).toBe(false);
	});
});
