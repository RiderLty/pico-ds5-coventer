// One shared device connection for the workspace and AI. The protocol module is
// imported from tools so this application and the diagnostic page stay in sync.
import { PicoDevice } from '../../../tools/pico-device.mjs';

export const SCRIPT_MAX_BYTES = 60 * 1024;

export class DeviceSession {
    constructor(device = new PicoDevice()) {
        this.device = device;
        this.listeners = new Set();
        this.logListeners = new Set();
        this.epoch = 0;
        this.operation = null;
        this.snapshot = { state: device.state, transport: null, busy: '', status: null, error: '', info: '', ramSource: null };
        device.onStateChange(state => {
            if (state === 'connecting' || state === 'disconnected') {
                this.epoch++;
                this.operation = null;
            }
            this.update({ state, transport: device.transportType,
                ...(state !== 'connected' ? { status: null, ramSource: null, info: '', busy: '' } : {}) });
        });
        device.onLog(text => { for (const fn of this.logListeners) fn(text); });
        device.onError(error => this.update({ error: error.message }));
        device.onClose(error => { if (error) this.update({ error: error.message }); });
    }

    update(patch) {
        this.snapshot = { ...this.snapshot, ...patch };
        for (const fn of this.listeners) fn();
    }
    subscribe = fn => { this.listeners.add(fn); return () => this.listeners.delete(fn); };
    onLog = fn => { this.logListeners.add(fn); return () => this.logListeners.delete(fn); };
    getSnapshot = () => this.snapshot;
    assertSession(epoch) {
        if (!this.device.connected || epoch !== this.epoch) throw new Error('设备连接已改变，请重新连接并读取脚本');
    }

    // Call synchronously from the click handler, preserving browser user activation.
    connect(kind, options) { return this.device.connect(kind, options); }
    disconnect() { return this.device.disconnect(); }

    async metadata(epoch, options) {
        const status = await this.device.scriptStatus(options);
        this.assertSession(epoch);
        const error = await this.device.scriptError(options);
        this.assertSession(epoch);
        this.update({ status, error });
        return status;
    }

    async perform(label, fn, options) {
        this.assertSession(this.epoch);
        if (this.operation) throw new Error('设备正在处理脚本，请等待当前操作完成');
        const epoch = this.epoch;
        const operation = Symbol(label);
        this.operation = operation;
        this.update({ busy: label, error: '' });
        try {
            const result = await fn(epoch);
            this.assertSession(epoch);
            return result;
        } catch (error) {
            if (this.device.connected && epoch === this.epoch) {
                // Failed RUN may have switched the board back to its built-in script.
                this.update({ ramSource: null });
                try { await this.metadata(epoch, options); } catch { /* preserve the original failure */ }
                if (epoch === this.epoch) this.update({ error: error.detail || error.message });
            }
            throw error;
        } finally {
            if (this.operation === operation) { this.operation = null; this.update({ busy: '' }); }
        }
    }

    read(options) {
        return this.perform('读取脚本', async epoch => {
            const source = await this.device.scriptRead(options);
            this.assertSession(epoch);
            await this.metadata(epoch, options);
            this.update({ ramSource: source });
            return source;
        }, options);
    }
    refresh(options) {
        return this.perform('刷新状态', async epoch => {
            const info = await this.device.info(options);
            this.assertSession(epoch);
            this.update({ info });
            return this.metadata(epoch, options);
        }, options);
    }
    run(source, options) {
        return this.perform('加载到内存', async epoch => {
            const status = await this.device.scriptRun(source, options);
            this.assertSession(epoch);
            this.update({ status, ramSource: source, error: '' });
            return status;
        }, options);
    }
    save(source, options) {
        return this.perform('写入存储', async epoch => {
            // SAVE persists the running image. Never upload or rerun as a side effect.
            const running = await this.device.scriptRead(options);
            this.assertSession(epoch);
            this.update({ ramSource: running });
            if (running !== source) throw new Error('编辑器与当前运行脚本不同，请先加载到内存并确认运行正常');
            await this.device.scriptSave(options);
            this.assertSession(epoch);
            return this.metadata(epoch, options);
        }, options);
    }
    reload(options) {
        return this.perform('加载 KV', async epoch => {
            await this.device.scriptReload(options);
            this.assertSession(epoch);
            const source = await this.device.scriptRead(options);
            this.assertSession(epoch);
            await this.metadata(epoch, options);
            this.update({ ramSource: source });
            return source;
        }, options);
    }
    restoreDefault(options) {
        return this.perform('运行预制脚本', async epoch => {
            await this.device.scriptDefault(options);
            this.assertSession(epoch);
            const source = await this.device.scriptRead(options);
            this.assertSession(epoch);
            await this.metadata(epoch, options);
            this.update({ ramSource: source });
            return source;
        }, options);
    }
    deleteSaved(options) {
        return this.perform('删除 KV', async epoch => {
            await this.device.scriptDelete(options);
            this.assertSession(epoch);
            return this.metadata(epoch, options);
        }, options);
    }
}

export const deviceSession = new DeviceSession();
