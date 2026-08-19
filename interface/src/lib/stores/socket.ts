import { writable } from 'svelte/store';
import msgpack from 'msgpack-lite';

export function createWebSocket() {
	let listeners = new Map<string, Set<(data?: unknown) => void>>();
	const { subscribe, set } = writable(false);
	const socketEvents = ['open', 'close', 'error', 'message', 'unresponsive'] as const;
	type SocketEvent = (typeof socketEvents)[number];
	let unresponsiveTimeoutId:  NodeJS.Timeout;
	let reconnectTimeoutId:  NodeJS.Timeout;
	let ws: WebSocket | undefined;
	let socketUrl: string | URL;
	let event_use_json = false;
	let generation = 0;

	function init(url: string | URL, use_json: boolean = false) {
		socketUrl = url;
		event_use_json = use_json;
		connect();
	}

	function disconnect(socket: WebSocket, socketGeneration: number, reason: SocketEvent, event?: Event) {
		if (ws !== socket || generation !== socketGeneration) return;
		//console.log('disconnect', reason, event);
		ws = undefined;
		if (closedByApp) {
			set(false);
			clearTimeout(unresponsiveTimeoutId);
			clearTimeout(reconnectTimeoutId);
			listeners.get(reason)?.forEach((listener) => listener(event));
			return;
		}
		if (socket.readyState === WebSocket.CONNECTING || socket.readyState === WebSocket.OPEN) {
			socket.close();
		}
		set(false);
		clearTimeout(unresponsiveTimeoutId);
		clearTimeout(reconnectTimeoutId);
		listeners.get(reason)?.forEach((listener) => listener(event));
		reconnectTimeoutId = setTimeout(connect, 1000);
	}

	let closedByApp = false;

	function close() {
		closedByApp = true;
		clearTimeout(unresponsiveTimeoutId);
		clearTimeout(reconnectTimeoutId);
		const socket = ws;
		ws = undefined;
		set(false);
		if (socket && (socket.readyState === WebSocket.CONNECTING || socket.readyState === WebSocket.OPEN)) {
			socket.close();
		}
	}

	function connect() {
		if (closedByApp) {
			closedByApp = false;
		}
		if (ws && (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)) return;
		//console.log('connect');
		const socket = new WebSocket(socketUrl);
		const socketGeneration = ++generation;
		ws = socket;
		socket.binaryType = 'arraybuffer';
		socket.onopen = (ev) => {
			if (ws !== socket || generation !== socketGeneration) return;
			set(true);
			clearTimeout(reconnectTimeoutId);
			for (const event of listeners.keys()) {
				if (socketEvents.includes(event as SocketEvent)) continue;
				if (!listeners.get(event)?.size) continue;
				sendEvent('subscribe', event);
			}
			listeners.get('open')?.forEach((listener) => listener(ev));
		};
		socket.onmessage = (message) => {
			if (ws !== socket || generation !== socketGeneration) return;
			resetUnresponsiveCheck(socket, socketGeneration);
			let payload = message.data;

			const binary = payload instanceof ArrayBuffer;
			listeners.get(binary ? 'binary' : 'message')?.forEach((listener) => listener(payload));
			
			try {
				payload = binary ? msgpack.decode(new Uint8Array(payload)) : JSON.parse(payload);
			} catch (error) {
				if (binary) {
					listeners.get('monitor')?.forEach((listener) => {
						listener(new Uint8Array(message.data));
					});
					return;
				}
				console.error('[WebSocket] Decode error:', error); // 🌙
				listeners.get('error')?.forEach((listener) => {
					listener(error);
				});
				return;
			}
			
			// 🌙 Non-object binary = raw monitor data (not msgpack); route directly and skip destructuring
			if (!payload || typeof payload !== 'object') {
				if (binary) {
					listeners.get('monitor')?.forEach((listener) => {
						listener(new Uint8Array(message.data));
					});
				} else {
					console.error('[WebSocket] Invalid payload:', payload);
				}
				return;
			}
			
			listeners.get('json')?.forEach((listener) => {
				listener(payload);
			});
			
			// Safe destructuring
			const { event = null, data = null } = payload;
			
			if (event) {
				// if (data !== null && data !== undefined) {
					listeners.get(event)?.forEach((listener) => {
						listener(data);
					});
				// }
			} else if (binary) { // 🌙 if no event, assume monitor data (raw binary)
				listeners.get('monitor')?.forEach((listener) => {
					listener(new Uint8Array(message.data));
				});
			} else {
				console.warn('[WebSocket] Missing "event" in non-binary payload:', payload);
 			}
		};
		socket.onerror = (ev) => disconnect(socket, socketGeneration, 'error', ev);
		socket.onclose = (ev) => disconnect(socket, socketGeneration, 'close', ev);
	}

	function unsubscribe(event: string, listener?: (data: any) => void) {
		let eventListeners = listeners.get(event);
		if (!eventListeners) return;

		if (listener) {
			eventListeners.delete(listener);
		} else {
			eventListeners.clear();
		}

		if (!eventListeners.size) {
			if (!socketEvents.includes(event as SocketEvent)) sendEvent('unsubscribe', event);
			listeners.delete(event);
		}
	}

	function resetUnresponsiveCheck(socket: WebSocket, socketGeneration: number) {
		clearTimeout(unresponsiveTimeoutId);
		unresponsiveTimeoutId = setTimeout(
			() => disconnect(socket, socketGeneration, 'unresponsive'),
			2000
		);
	}

	function send(msg: unknown) {
		if (!ws || ws.readyState !== WebSocket.OPEN) return;
		if (event_use_json) {
			ws.send(JSON.stringify(msg));
		} else {
			ws.send(msgpack.encode(msg));
		}
	}

	function sendEvent(event: string, data: unknown) {
		send({ event, data });
	}

	return {
		subscribe,
		send,
		sendEvent,
		init,
		close,
		on: <T>(event: string, listener: (data: T) => void): (() => void) => {
			let eventListeners = listeners.get(event);
			if (!eventListeners) {
				eventListeners = new Set();
				listeners.set(event, eventListeners);
				
				// Only send subscription if WebSocket is open and it's not a socket event
				if (!socketEvents.includes(event as SocketEvent) && 
					ws && ws.readyState === WebSocket.OPEN) {
					sendEvent('subscribe', event);
				}
			}
			eventListeners.add(listener as (data: any) => void);

			return () => {
				unsubscribe(event, listener);
			};
		},
		off: (event: string, listener?: (data: any) => void) => {
			unsubscribe(event, listener);
		}
	};
}

export const socket = createWebSocket();
