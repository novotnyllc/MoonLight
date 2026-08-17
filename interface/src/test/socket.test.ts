import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import msgpack from 'msgpack-lite';
import { createWebSocket } from '../lib/stores/socket';

class MockWebSocket {
	static readonly CONNECTING = 0;
	static readonly OPEN = 1;
	static readonly CLOSING = 2;
	static readonly CLOSED = 3;
	static instances: MockWebSocket[] = [];

	readyState = MockWebSocket.CONNECTING;
	binaryType = '';
	onopen: ((event: Event) => void) | null = null;
	onmessage: ((event: MessageEvent) => void) | null = null;
	onerror: ((event: Event) => void) | null = null;
	onclose: ((event: CloseEvent) => void) | null = null;
	sent: unknown[] = [];
	closeCount = 0;

	constructor(readonly url: string | URL) {
		MockWebSocket.instances.push(this);
	}

	send(data: unknown) {
		this.sent.push(data);
	}

	close() {
		this.closeCount++;
		this.readyState = MockWebSocket.CLOSING;
	}
}

describe('socket lifecycle', () => {
	beforeEach(() => {
		vi.useFakeTimers();
		MockWebSocket.instances = [];
		vi.stubGlobal('WebSocket', MockWebSocket);
	});

	afterEach(() => {
		vi.useRealTimers();
		vi.unstubAllGlobals();
	});

	it('ignores stale generation callbacks and does not duplicate an active connection', () => {
		const socket = createWebSocket();
		socket.init('ws://device/ws/events');
		const first = MockWebSocket.instances[0];
		const staleClose = first.onclose!;

		first.readyState = MockWebSocket.CLOSED;
		staleClose(new Event('close') as CloseEvent);
		vi.advanceTimersByTime(1000);

		const current = MockWebSocket.instances[1];
		current.readyState = MockWebSocket.OPEN;
		current.onopen!(new Event('open'));
		staleClose(new Event('close') as CloseEvent);
		socket.init('ws://device/ws/events');
		vi.advanceTimersByTime(1000);

		expect(MockWebSocket.instances).toHaveLength(2);
		expect(current.closeCount).toBe(0);
	});

	it('unsubscribes and removes the final listener', () => {
		const socket = createWebSocket();
		socket.init('ws://device/ws/events');
		const current = MockWebSocket.instances[0];
		current.readyState = MockWebSocket.OPEN;
		current.onopen!(new Event('open'));

		const off = socket.on('monitor', () => undefined);
		off();

		expect(current.sent).toHaveLength(2);
		expect(msgpack.decode(current.sent[1] as Uint8Array)).toEqual({
			event: 'unsubscribe',
			data: 'monitor'
		});
	});
});
