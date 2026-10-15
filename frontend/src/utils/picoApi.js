// Adapter used by the AI tools; device operations stay on the shared session.
import { deviceSession } from './deviceSession';

export async function luaSet(source, options) {
    const status = await deviceSession.run(source, options);
    return { ok: true, status, message: '已加载到内存' };
}
export async function luaState() {
    const state = deviceSession.getSnapshot();
    if (state.state !== 'connected') throw new Error('设备未连接');
    return { ...state.status, loaded: !!state.status, error: state.error,
        source: state.status?.default ? 'default' : 'ram' };
}
