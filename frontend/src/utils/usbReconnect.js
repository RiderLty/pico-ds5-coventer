import { PicoDevice } from '../../../tools/pico-device.mjs';

const KEY = 'ds5_usb_reconnect_v1';
const RETRY_MS = [250, 500, 1000, 2000, 4000, 8000];
const browserStorage = () => { try { return globalThis.localStorage; } catch { return null; } };

// Only getDevices(): reconnect must never open a browser permission picker.
export class UsbReconnect {
    constructor(session, { usb = globalThis.navigator?.usb, storage = browserStorage(),
        page = globalThis.document, schedule = (fn, ms) => setTimeout(fn, ms), cancel = id => clearTimeout(id) } = {}) {
        Object.assign(this, { session, usb, storage, page, schedule, cancel });
        this.preference = { enabled: true, target: null };
        try {
            const saved = JSON.parse(storage?.getItem(KEY) || 'null');
            if (saved && typeof saved.enabled === 'boolean') this.preference = saved;
        } catch { /* Storage unavailable: remember only for this page. */ }
        this.version = 0;
        this.attempt = 0;
        this.active = false;
        this.running = false;
        this.manual = false;
        this.automatic = false;
    }

    status(message) {
        if (this.session.getSnapshot().reconnectStatus !== message) this.session.update({ reconnectStatus: message });
    }
    invalidate() {
        ++this.version;
        this.cancel(this.timer);
        this.timer = null;
        this.wakePending = false;
    }
    save() {
        try { this.storage?.setItem(KEY, JSON.stringify(this.preference)); } catch { /* Memory still works. */ }
    }
    manualStart() {
        this.invalidate();
        this.manual = true;
        this.status('');
    }
    manualDone(kind, usbDevice) {
        this.manual = false;
        if (kind === 'winusb') {
            this.lastDevice = usbDevice;
            this.preference = { enabled: true, target: usbDevice ? {
                vendorId: usbDevice.vendorId, productId: usbDevice.productId, serialNumber: usbDevice.serialNumber || '',
            } : null };
        } else this.preference = { ...this.preference, enabled: false };
        this.save();
    }
    disconnect() {
        this.invalidate();
        this.manual = false;
        this.preference = { ...this.preference, enabled: false };
        this.save();
        this.status('自动连接已暂停；点击连接 USB 可恢复');
    }
    available() {
        return this.active && !this.manual && this.preference.enabled && this.session.device.state === 'disconnected';
    }
    later(delay) {
        if (!this.available()) return;
        this.cancel(this.timer);
        this.timer = this.schedule(() => { this.timer = null; void this.scan(); }, delay);
    }
    wake = () => {
        if (!this.available()) return;
        this.attempt = 0;
        if (this.running) this.wakePending = true;
        else this.later(0);
    };

    start() {
        if (this.active || !this.usb?.getDevices) return () => {};
        this.active = true;
        this.usb.addEventListener('connect', this.wake);
        this.visibility = () => { if (this.page.visibilityState === 'visible') this.wake(); };
        this.page?.addEventListener('visibilitychange', this.visibility);
        let previous = this.session.device.state;
        this.unsubscribe = this.session.subscribe(() => {
            const state = this.session.device.state;
            if (state === previous) return;
            previous = state;
            if (state === 'connected') this.status('');
            if (state === 'disconnected' && !this.automatic) this.wake();
        });
        if (!this.preference.enabled) this.status('自动连接已暂停；点击连接 USB 可恢复');
        this.wake();
        return () => this.stop();
    }
    stop() {
        this.active = false;
        this.invalidate();
        if (this.automatic && this.session.device.state === 'connecting') {
            void this.session.device.disconnect().catch(() => {});
        }
        this.usb?.removeEventListener('connect', this.wake);
        this.page?.removeEventListener('visibilitychange', this.visibility);
        this.unsubscribe?.();
    }

    async scan() {
        if (!this.available() || this.running) return;
        const version = this.version;
        this.running = true;
        let retry = false;
        try {
            const devices = await this.usb.getDevices();
            if (version !== this.version || !this.available()) return;
            const target = this.preference.target;
            const matches = devices.filter(device => PicoDevice.isWinUsbDevice(device) && (!target ||
                (device.vendorId === target.vendorId && device.productId === target.productId &&
                    (!target.serialNumber || device.serialNumber === target.serialNumber))));
            const candidate = matches.includes(this.lastDevice) ? this.lastDevice : matches.length === 1 ? matches[0] : null;
            if (!candidate) {
                this.status(matches.length > 1 ? '发现多个已授权设备，请手动选择 USB 设备' : '等待已授权的 WinUSB 设备；未找到时请点击连接 USB');
                retry = matches.length === 0;
                return;
            }
            this.status('正在自动连接 USB…');
            this.automatic = true;
            try {
                // Bypass the manual-connect wrapper; PicoDevice still performs RESET.
                await this.session.device.connect('winusb', { device: candidate });
                if (version === this.version && this.session.device.connected) {
                    this.lastDevice = candidate;
                    this.preference = { enabled: true, target: { vendorId: candidate.vendorId,
                        productId: candidate.productId, serialNumber: candidate.serialNumber || '' } };
                    this.save();
                    this.session.update({ error: '' });
                    this.status('');
                }
            } finally { this.automatic = false; }
        } catch (error) {
            if (version === this.version && this.available()) {
                this.status(`USB 自动连接失败：${error.message}；可点击连接 USB 重试`);
                retry = true;
            }
        } finally {
            this.running = false;
            if (this.wakePending) { this.wakePending = false; this.wake(); }
            else if (version === this.version && retry && this.attempt < RETRY_MS.length) this.later(RETRY_MS[this.attempt++]);
        }
    }
}
