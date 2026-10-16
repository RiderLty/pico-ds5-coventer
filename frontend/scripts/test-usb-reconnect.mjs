import assert from 'node:assert/strict';
import { DeviceSession } from '../src/utils/deviceSession.js';
import { UsbReconnect } from '../src/utils/usbReconnect.js';
import { PicoDevice } from '../../tools/pico-device.mjs';

const KEY = 'ds5_usb_reconnect_v1';
const settle = async () => { for (let i=0;i<30;i++) await Promise.resolve(); };
const deferred = () => { let resolve; const promise = new Promise(r => { resolve=r; }); return { promise, resolve }; };
const board = (serialNumber = '') => ({ vendorId: 0x054c, productId: 0x0ce6, serialNumber,
    configurations: [{ interfaces: [{ interfaceNumber: 4, alternates: [{ interfaceClass: 255, endpoints: [
        { type: 'bulk', direction: 'in' }, { type: 'bulk', direction: 'out' },
    ] }] }] }] });
class FakeDevice {
    state = 'disconnected';
    calls = [];
    get connected() { return this.state === 'connected'; }
    onStateChange(fn) { this.changed = fn; }
    onLog() {} onError(fn) { this.error = fn; } onClose() {}
    stateTo(state) { this.state=state; this.changed(state); }
    async connect(kind, options) {
        this.calls.push({ kind, options }); this.stateTo('connecting');
        if (this.pending) {
            await this.pending.promise;
            if (this.state !== 'connecting') throw new Error('cancelled');
        }
        if (this.fail) {
            this.stateTo('disconnected'); this.error(new Error('driver busy')); throw new Error('driver busy');
        }
        this.transportType=kind; this.usbDevice=options?.device; this.stateTo('connected');
    }
    async disconnect() { this.stateTo('disconnecting'); this.stateTo('disconnected'); }
}
function setup(saved = null) {
    const data = new Map(saved ? [[KEY, JSON.stringify(saved)]] : []);
    const storage = { getItem: k => data.get(k), setItem: (k,v) => data.set(k,v) };
    const tasks = new Map(); let id=0;
    const usb = new EventTarget(); usb.devices=[]; usb.scans=0;
    usb.getDevices = async () => { ++usb.scans; return usb.pending ? usb.pending.promise : usb.devices; };
    usb.requestDevice = () => { throw new Error('Automatic reconnect must never open a picker'); };
    const fake = new FakeDevice(); const session = new DeviceSession(fake);
    const controller = new UsbReconnect(session, { usb, storage, page: null,
        schedule: (fn,ms) => { tasks.set(++id,{fn,ms}); return id; }, cancel: i => tasks.delete(i) });
    session.usbReconnect=controller;
    const step = async () => {
        const entry=tasks.entries().next().value;
        assert.ok(entry, 'expected a scheduled scan'); tasks.delete(entry[0]); entry[1].fn(); await settle();
    };
    return { usb, fake, session, controller, tasks, data, step };
}

// Descriptor matching excludes genuine HID DS5 and other vendor/product devices.
const hid = board(); hid.configurations[0].interfaces[0].alternates[0].interfaceClass=3;
const other = board(); other.productId=123;
assert.equal(PicoDevice.isWinUsbDevice(hid), false);
assert.equal(PicoDevice.isWinUsbDevice(other), false);
const oneWay = board(); oneWay.configurations[0].interfaces[0].alternates[0].endpoints.pop();
assert.equal(PicoDevice.isWinUsbDevice(oneWay), false);
assert.equal(PicoDevice.isWinUsbDevice(board()), true);
{
    const t=setup(); t.usb.devices=[hid,other]; t.controller.start();
    await t.step(); assert.equal(t.fake.calls.length,0);
    for(let i=0;i<6;i++) await t.step();
    assert.equal(t.tasks.size,0); // bounded backoff; no endless open/error loop
    t.usb.devices.push(board('A')); t.usb.dispatchEvent(new Event('connect')); await t.step();
    assert.equal(t.fake.calls.length,1); assert.equal(t.fake.connected,true);
    assert.equal(JSON.parse(t.data.get(KEY)).target.serialNumber,'A');
    assert.equal(t.session.getSnapshot().reconnectStatus,'');
    t.controller.stop(); assert.equal(t.tasks.size,0);
}
{
    const t=setup({ enabled:true,target:{vendorId:0x054c,productId:0x0ce6,serialNumber:'A'} });
    const a=board('A'); t.usb.devices=[board('B'),a]; t.controller.start(); await t.step();
    assert.equal(t.fake.calls[0].options.device,a);
    await t.fake.disconnect(); await t.step(); // unexpected disconnect reconnects
    assert.equal(t.fake.calls.length,2);
    await t.session.disconnect(); t.usb.dispatchEvent(new Event('connect')); await settle();
    assert.equal(t.tasks.size,0); assert.equal(t.fake.calls.length,2);
    t.controller.stop(); t.controller.start(); assert.equal(t.tasks.size,0); // pause survives reload
    await t.session.connect('winusb',{device:a});
    await t.fake.disconnect(); await t.step(); assert.equal(t.fake.calls.length,4);
    await t.session.disconnect(); await t.session.connect('serial');
    await t.fake.disconnect(); assert.equal(t.tasks.size,0); // UART choice takes precedence
    t.controller.stop();
}
{
    const t=setup(); t.usb.devices=[board(),board()]; t.controller.start(); await t.step();
    assert.equal(t.fake.calls.length,0); assert.match(t.session.getSnapshot().reconnectStatus,/多个/);
    t.controller.stop();
}
{
    const t=setup(); t.usb.devices=[board('A')]; t.fake.fail=true; t.controller.start(); await t.step();
    assert.equal(t.fake.calls.length,1); assert.match(t.session.getSnapshot().reconnectStatus,/driver busy/);
    assert.equal(t.session.getSnapshot().error,''); // background retry does not spam errors
    t.fake.fail=false; await t.step(); assert.equal(t.fake.connected,true);
    t.controller.stop();
}
{
    const t=setup(); t.usb.pending=deferred(); t.controller.start(); await t.step();
    t.usb.dispatchEvent(new Event('connect')); t.usb.dispatchEvent(new Event('connect'));
    t.usb.pending.resolve([board('A')]); await settle();
    assert.equal(t.fake.calls.length,1); assert.equal(t.tasks.size,0); // concurrent discovery is serialized
    t.controller.stop();
}
for (const action of ['stop','manualStart','disconnect']) {
    const t=setup(); t.usb.pending=deferred(); t.controller.start(); await t.step();
    t.controller[action](); t.usb.pending.resolve([board('A')]); await settle();
    assert.equal(t.fake.calls.length,0); assert.equal(t.tasks.size,0); t.controller.stop();
}
{
    const t=setup(); t.usb.devices=[board('A')]; t.fake.pending=deferred();
    t.controller.start(); await t.step(); assert.equal(t.fake.state,'connecting');
    t.controller.stop(); t.fake.pending.resolve(); await settle();
    assert.equal(t.fake.state,'disconnected'); assert.equal(t.tasks.size,0);
}
{
    const t=setup(); t.usb.getDevices=async () => { throw new Error('permission denied'); };
    t.controller.start(); await t.step(); assert.match(t.session.getSnapshot().reconnectStatus,/permission denied/);
    t.controller.stop();
}
console.log('PASS: WebUSB authorized discovery, HID exclusion, saved identity, hotplug/reload, retries, ambiguity, manual pause/UART, stale scans and cleanup');
