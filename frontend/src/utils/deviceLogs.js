import { deviceSession } from './deviceSession';
import { pushLog } from './logStore';

// PRINT messages may split a logical line. Emit unterminated fragments promptly
// so a long-running script's output remains visible; cap the carry buffer.
export function attachDeviceLogs(session, append = pushLog) {
    let carry = '';
    let timer;
    const flush = () => {
        clearTimeout(timer);
        if (carry) { append(carry); carry = ''; }
    };
    const offLog = session.onLog(text => {
        carry += text;
        const lines = carry.split(/\r?\n/);
        carry = lines.pop();
        for (const line of lines) if (line) append(line);
        if (carry.length > 4096) flush();
        clearTimeout(timer);
        if (carry) timer = setTimeout(flush, 100);
    });
    const offState = session.subscribe(() => { if (session.getSnapshot().state === 'disconnected') flush(); });
    return () => { offLog(); offState(); flush(); };
}

attachDeviceLogs(deviceSession);
