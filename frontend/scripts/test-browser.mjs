import { chromium, expect } from '@playwright/test';
const browser = await chromium.launch({ headless: true,
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}) });
try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.addInitScript(() => {
        localStorage.setItem('ds5_lua_draft_v1', JSON.stringify({ script: 'stale draft' }));
        localStorage.setItem('ds5_left_panel_open_v1', '1');
        localStorage.setItem('ds5_left_panel_section_v1', 'device');
    });
    await page.goto(process.env.WORKSPACE_TEST_URL || 'http://127.0.0.1:5173/');
    const editor = page.locator('.cm-content');
    const aiButton = page.getByRole('button', { name: /^AI 助手/ });
    await expect(editor).toHaveAttribute('contenteditable', 'false');
    await expect(aiButton).toBeDisabled();
    await expect(page.getByRole('button', { name: '加载到内存', exact: true })).toBeDisabled();
    await expect(editor).not.toContainText('stale draft');
    await page.evaluate(async () => {
        const url=performance.getEntriesByType('resource').find(entry => /\/src\/utils\/deviceSession\.js(?:\?|$)/.test(entry.name))?.name;
        const { deviceSession: s } = await import(url || '/src/utils/deviceSession.js');
        window.testSession = s;
        window.testSource = 'print("device A")';
        s.device.connect = async () => { s.device._setState('connecting'); s.device._setState('connected'); };
        s.device.disconnect = async () => { s.device._setState('disconnecting'); s.device._setState('disconnected'); };
        s.device.scriptRead = async () => {
            if (window.holdRead) await new Promise(resolve => { window.releaseRead = resolve; });
            if (window.failRead) throw new Error('read failed');
            return window.testSource;
        };
        s.device.scriptStatus = async () => ({ kvReady: true, dirty: false, partitionSize: 524288 });
        s.device.scriptError = async () => '';
        s.device.aiCfgGet = async () => ({ apiKey: 'test-key', model: 'test-model' });
        s.device.info = async () => 'test board';
        window.holdRead = true;
    });
    const connect = () => page.getByRole('button', { name: '连接 USB', exact: true }).click();
    await connect();
    await expect(editor).toHaveAttribute('contenteditable', 'false');
    await expect(aiButton).toBeDisabled();
    await page.waitForFunction(() => !!window.releaseRead);
    await page.evaluate(() => { window.holdRead = false; window.releaseRead(); });
    await expect(editor).toContainText('device A');
    await expect(editor).toHaveAttribute('contenteditable', 'true');
    await expect(aiButton).toBeEnabled();
    await editor.fill('print("local edit")');
    await page.getByRole('button', { name: '断开', exact: true }).click();
    await expect(editor).toContainText('local edit');
    await expect(editor).toHaveAttribute('contenteditable', 'false');
    await expect(aiButton).toBeDisabled();
    await editor.press('Meta+z');
    await expect(editor).toContainText('local edit');
    await page.getByRole('button', { name: '脚本管理', exact: true }).click();
    await expect(page.getByRole('button', { name: '导出当前脚本', exact: true })).toBeEnabled();
    await page.getByRole('button', { name: '设备', exact: true }).click();
    await page.evaluate(() => { window.testSource = 'print("device B")'; });
    await connect();
    await expect(editor).toHaveAttribute('contenteditable', 'true');
    await expect(editor).toContainText('local edit');
    await expect(page.locator('.workspace-editor-status')).toContainText('不同');
    await page.getByText('读取设备', { exact: true }).click();
    await page.getByRole('button', { name: '确认', exact: true }).click();
    await expect(editor).toContainText('device B');
    await page.getByText('运行预制脚本', { exact: true }).click();
    await page.evaluate(() => window.testSession.disconnect());
    await expect(page.getByRole('button', { name: '确认', exact: true })).toHaveCount(0);
    await page.evaluate(() => { window.testSource = 'print("device C")'; window.failRead = true; });
    await connect();
    await expect(page.getByRole('alert')).toContainText('read failed');
    await expect(editor).toHaveAttribute('contenteditable', 'false');
    await expect(editor).toContainText('device B');
    await page.evaluate(() => { window.failRead = false; });
    await page.getByRole('button', { name: '刷新', exact: true }).click();
    await expect(editor).toContainText('device C');
    await expect(editor).toHaveAttribute('contenteditable', 'true');
    if (await page.locator('.workspace-ai').isHidden()) await aiButton.click();
    await page.evaluate(() => {
        window.fetch = (url, options) => new Promise((resolve, reject) => {
            window.aiStarted = true;
            options.signal.addEventListener('abort', () => { window.aiAborted = true; reject(new DOMException('Aborted', 'AbortError')); });
        });
    });
    const input = page.locator('.workspace-ai textarea').first();
    await input.fill('读取当前脚本'); await input.press('Enter');
    await page.waitForFunction(() => window.aiStarted);
    await page.evaluate(() => window.testSession.disconnect());
    await page.waitForFunction(() => window.aiAborted);
    await expect(editor).toContainText('device C');
    await expect(editor).toHaveAttribute('contenteditable', 'false');
    await expect(page.locator('.workspace-ai textarea')).toHaveCount(0);
    expect(errors).toEqual([]);
    console.log('PASS: browser offline gates, initial read, retained edits/reconnect, explicit replace, confirmations, read failure/retry, AI abort');

    // Real App/UsbReconnect lifecycle, with only browser USB and wire calls mocked.
    const auto = await browser.newPage();
    const autoErrors=[];
    auto.on('pageerror', error => autoErrors.push(error.message));
    await auto.addInitScript(() => {
        localStorage.setItem('ds5_left_panel_open_v1','1');
        localStorage.setItem('ds5_left_panel_section_v1','device');
        const usb=new EventTarget();
        const board={vendorId:0x054c,productId:0x0ce6,serialNumber:'PICO-A',configurations:[{interfaces:[{
            interfaceNumber:4,alternates:[{interfaceClass:255,endpoints:[{type:'bulk',direction:'in'},{type:'bulk',direction:'out'}]}],
        }]}]};
        const hid=structuredClone(board); hid.configurations[0].interfaces[0].alternates[0].interfaceClass=3;
        usb.devices=localStorage.getItem('test_usb_authorized') ? [hid,board] : [hid];
        usb.getDevices=async () => usb.devices;
        usb.requestDevice=async () => { throw new Error('unexpected permission picker'); };
        Object.defineProperty(navigator,'usb',{configurable:true,value:usb});
        window.usbBoard=board;
    });
    await auto.route(/\/src\/utils\/deviceSession\.js(?:\?.*)?$/, async route => {
        const response=await route.fetch();
        await route.fulfill({response,body:await response.text()+`
            window.testSession=deviceSession; window.autoCalls=0; window.testSource='print("auto board")';
            const d=deviceSession.device;
            d.connect=async (kind,options) => {
                ++window.autoCalls;
                d._session={kind,transport:{device:options.device}};
                d._setState('connecting'); d._setState('connected');
            };
            d.disconnect=async () => { d._setState('disconnecting'); d._session=null; d._setState('disconnected'); };
            d.scriptRead=async () => window.testSource;
            d.scriptStatus=async () => ({kvReady:true,dirty:false});
            d.scriptError=async () => '';
            d.aiCfgGet=async () => ({apiKey:'auto-key',model:'auto-model'});
        `});
    });
    await auto.goto(process.env.WORKSPACE_TEST_URL || 'http://127.0.0.1:5173/');
    const autoEditor=auto.locator('.cm-content');
    await expect(autoEditor).toHaveAttribute('contenteditable','false');
    expect(await auto.evaluate(() => window.autoCalls)).toBe(0); // genuine HID DS5 ignored
    await auto.evaluate(() => {
        localStorage.setItem('test_usb_authorized','1');
        navigator.usb.devices.push(window.usbBoard); navigator.usb.dispatchEvent(new Event('connect'));
    });
    await expect(autoEditor).toContainText('auto board');
    await expect(autoEditor).toHaveAttribute('contenteditable','true');
    await expect(auto.getByRole('button',{name:/^AI 助手/})).toBeEnabled();
    await autoEditor.fill('print("retained after USB reload")');
    await auto.evaluate(async () => {
        navigator.usb.devices=[]; await window.testSession.device.disconnect();
    });
    await expect(autoEditor).toHaveAttribute('contenteditable','false');
    await expect(autoEditor).toContainText('retained after USB reload');
    await auto.evaluate(() => {
        window.testSource='print("new device script")';
        navigator.usb.devices=[structuredClone(window.usbBoard)];
        navigator.usb.dispatchEvent(new Event('connect'));
    });
    await expect(autoEditor).toHaveAttribute('contenteditable','true');
    await expect(autoEditor).toContainText('retained after USB reload');
    expect(await auto.evaluate(() => window.autoCalls)).toBe(2);
    // Refresh restores authorization and the device source; no picker/click needed.
    auto.on('dialog',dialog => dialog.accept());
    await auto.reload();
    await expect(autoEditor).toContainText('auto board');
    await expect(autoEditor).toHaveAttribute('contenteditable','true');
    expect(await auto.evaluate(() => window.autoCalls)).toBe(1);
    await auto.getByRole('button',{name:'断开',exact:true}).click();
    await expect(auto.getByRole('status')).toContainText('自动连接已暂停');
    await auto.reload();
    await expect(autoEditor).toHaveAttribute('contenteditable','false');
    await expect(auto.getByRole('status')).toContainText('自动连接已暂停');
    expect(await auto.evaluate(() => window.autoCalls)).toBe(0);
    expect(autoErrors).toEqual([]);
    await auto.close();
    console.log('PASS: browser automatic WinUSB discovery, HID exclusion, hotplug retains edits, AI reload, page refresh and persistent manual disconnect');
} finally { await browser.close(); }
